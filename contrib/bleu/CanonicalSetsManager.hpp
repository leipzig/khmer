/*
 *  CanonicalSetManager.h
 *  bleu
 *
 *  Created by Rosangela Canino-Koning on 10/8/10.
 *  Copyright 2010 __MyCompanyName__. All rights reserved.
 *
 */

#include "../../lib/hashtable.hh"
#include "../../lib/parsers.hh"
#include "../external_lib/cBitArray.h"
#include "CanonicalSet.hpp"
#include <algorithm>

#define BIT_COUNT_PARTITION 1000

#define HASHES 8
#define BIN_SIZE 50
#define FOSTER_COUNT_MULTIPLIER 5

#define CACHESIZE 10

#define SETS_SIZE 65535

namespace bleu {
  
  using namespace khmer;
  using namespace std;
  
  typedef CanonicalSet * SetHandle;
  typedef SetHandle * SetPointer;
  
  typedef unsigned long long HashBin;
  typedef unsigned long long SetOffsetBin;
  typedef unsigned short SetOffset;
  
  class CanonicalSetsManager
  {
  private:
    // sizes set during constructor
    cBitArray * _hash_table_preliminary[HASHES]; // two-dimensional hash-table
    cBitArray * _hash_table[HASHES]; // two-dimensional hash-table
    
    unsigned long long _tablesizes[HASHES]; // the sizes of the hash tables
    unsigned long long * _hash_table_bit_counts_lookup[HASHES]; // the number of bits set in the hash table, by every 10k entries
    unsigned int _last_set_offset;
    
    // sizes set during prep 1 (based on processing of _hash_table)
    unsigned long long _hash_table_total_bit_counts[HASHES];
    unsigned short * _set_offsets[HASHES]; 
    
    // contents set during (based on the processing of the reads)  
    vector<SetPointer> _sets; // array of sets
    
    
    // caching system so I don't have to re-modulo. Dunno if this is faster than modulo or not. will test.
    pair<HashIntoType, HashBin> _hash_bin_cache[HASHES][CACHESIZE];
    int _hash_bin_cache_last_used_index[HASHES];
    
    // caching system so I don't have to keep re-counting the bits. Dunno if this is faster than counting or not.
    pair<HashBin, SetOffsetBin> _set_offset_bin_cache[HASHES][CACHESIZE];
    int _set_offset_bin_cache_last_used_index[HASHES];
    
    vector<SetOffset> _released_set_offsets;
    
    //map<unsigned long long, list<SetHandle*> > _binned_sets; // our sets, sorted by size, in heap form
    vector<SetPointer> _sorted_sets;
    
  public:
    CanonicalSetsManager( unsigned long long aMaxMemory )
    {
      _last_set_offset = 0; // init to zero
      
      _tablesizes[0] = get_first_prime_below( aMaxMemory / HASHES );
      for ( int i = 1; i < HASHES; ++i )      
      {
        _tablesizes[i] = get_first_prime_below(_tablesizes[i-1]); 
      }
      
      for ( int j = 0; j < HASHES; ++j )      
      {
        _hash_table_preliminary[j] = new cBitArray( _tablesizes[j] );
        _hash_table_preliminary[j]->Clear();      
        
        _hash_table[j] = new cBitArray( _tablesizes[j] );
        _hash_table[j]->Clear(); 
        
        _hash_table_bit_counts_lookup[j] = new unsigned long long[(_tablesizes[j] / BIT_COUNT_PARTITION)+1];
        memset(_hash_table_bit_counts_lookup[j], 0, ((_tablesizes[j] / BIT_COUNT_PARTITION)+1) * sizeof(unsigned long long));
        
        // THESE WILL GET SET LATER.            
        _hash_table_total_bit_counts[j] = 0;
        _set_offsets[j] = NULL; // a table the size of the number of bits in the hash table.
        
        _hash_bin_cache_last_used_index[j] = 0;
        memset(_hash_bin_cache[j], 0, sizeof(pair<HashIntoType,HashBin>) * CACHESIZE);
        
        _set_offset_bin_cache_last_used_index[j] = 0;
        memset(_set_offset_bin_cache[j], 0, sizeof(pair<HashBin,SetOffsetBin>) * CACHESIZE);
      }
      
      _sets.resize(SETS_SIZE, NULL);
    }
    //
    // first pass through the reads -- determine if reads are interesting 
    //   enough to warrant getting space to call out a set
    //    
    
    // mark a kmer/hash as "interesting" if it appears more than once.
    void seen_hash( HashIntoType aHash )
    {
      for (int i = 0; i < HASHES; ++i )
      {
        unsigned long long lHashBin = HashToHashBinCached(aHash, i); 
        
        if ( _hash_table_preliminary[i]->Get(lHashBin) == true )
          _hash_table[i]->Set(lHashBin, true);
        else
          _hash_table_preliminary[i]->Set(lHashBin, true);
      }
    }
    
    //
    // second pass through the reads -- actually perform the set assignments
    //
    
    // check whether a kmer can have a set. (I keep wanting to type can_has_set) Damned lolcats.
    bool can_have_set( HashIntoType aHash )
    {
      bool lCanHaveASet = false;
      for ( int i = 0; i < HASHES; ++i )
      {
        HashBin lHashBin = HashToHashBinCached(aHash, i);
        
        if ( i == 0 )
          lCanHaveASet = _hash_table[i]->Get(lHashBin) == true;
        else if ( _hash_table[i]->Get(lHashBin) != lCanHaveASet ) // we have a disagreement.
          return false;
      }
      
      return lCanHaveASet;
    }    
    // find a set for this hash. if there isn't one, create it.
    SetHandle get_set( HashIntoType aHash )
    { 
      SetHandle lHandle = NULL;
      
      if ( has_existing_set( aHash ) )
        lHandle = get_existing_set( aHash );
      else
      {
        lHandle = create_set();
        add_to_set(lHandle, aHash);
      }
      
      
      return lHandle;
    }
    // check whether a kmer has already been assigned a set.
    bool has_existing_set( HashIntoType aHash )
    {
      for ( int i = 0; i < HASHES; ++i )
      {
        SetOffsetBin lBin = HashBinToSetOffsetBinCached( HashToHashBinCached(aHash, i), i );
        if ( _set_offsets[i][ lBin ] == 0 )
          return false;
      }
      
      return true;
    }
    // find a set for this hash that already exists.
    SetHandle get_existing_set( HashIntoType aHash )
    {      
      map<SetHandle, int> lRepresented;
      
      for ( int i = 0; i < HASHES; ++i )
      {
        SetOffsetBin lBin = HashBinToSetOffsetBinCached( HashToHashBinCached(aHash, i), i );
        
        assert ( _set_offsets[i][ lBin ] > 0 ); // somehow we fucked this up.
        
        lRepresented[ SetOffsetToSet( _set_offsets[i][ lBin ] ) ]++;
      }
      
      // figure out what the consensus set was
      SetHandle lMostRepresented = NULL; 
      for ( map<SetHandle, int>::iterator lSet = lRepresented.begin(); lSet != lRepresented.end(); ++lSet ) // count the votes
      {
        if ( lMostRepresented == NULL || lSet->second > lRepresented[ lMostRepresented ] ) // because maps are sorted, this will end up being the set with the lowest pointer, and the highest count.
          lMostRepresented = lSet->first;
      }     
      
      return lMostRepresented;      
    }
    bool sets_are_disconnected( SetHandle aSet1, SetHandle aSet2 )
    {
      if ( aSet1 == aSet2 )
        return false; // they're already in the same set.
      else
        return true; // one of them is 
    }    
    // put together two sets.
    SetHandle bridge_sets( SetHandle aEncounteredSet, SetHandle aOriginatingSet )
    {
      assert ( aOriginatingSet != aEncounteredSet ); // we really really shouldn't be the same set.
      
      SetHandle lDominatingSet = NULL;
      
      //cout << "originating set: "; aOriginatingSet->output_info();
      //cout << "encountered set: "; aEncounteredSet->output_info();
      
      // now, figure out which one should join to which.
//      // first, if neither of them are fosters, we can just have the larger set adopt the smaller set.
//      if ( !aOriginatingSet->AmFostered() &&
//          !aEncounteredSet->AmFostered() ) // we're not foster children!
//      { 
        if ( aOriginatingSet->GetPrimaryOffset() < aEncounteredSet->GetPrimaryOffset() )
        {
          join( aOriginatingSet, aEncounteredSet );
          lDominatingSet = aOriginatingSet;
        }
        else
        {
          join( aEncounteredSet, aOriginatingSet );
          lDominatingSet = aEncounteredSet;
        }     
//      }
//      else
//      {        
//        // since either one, or both are rooted in fosteredes, we don't join the entire sets, but only the branch of the family rooted up to there.
//        if ( !aOriginatingSet->AmFostered() &&
//            aEncounteredSet->AmFostered() ) // originating set isn't fostered, but encountered is.
//        {
//          // send the encountered set's entire biological tree back to his biological family.
//          adopt( aOriginatingSet, aEncounteredSet->BiologicalPatriarch );        
//          lDominatingSet = aOriginatingSet;
//        }
//        else // the other way around, or they're both fostered, so, makes no difference.
//        {
//          adopt( aEncounteredSet, aOriginatingSet->BiologicalPatriarch ); 
//          lDominatingSet = aEncounteredSet;
//        } 
//      }
      
      //cout << "dominating set post-bridge: "; lDominatingSet->output_info();
      
      return lDominatingSet;
    }    
    void canonicalize()
    {
      // go through all the set offsets and point them to their canonical locations.
      for ( int i = 0; i < HASHES; ++i )
      {
        for ( int j = 0; j < _hash_table_total_bit_counts[i]; ++j )
        {
          if ( _set_offsets[i][j] != 0 ) // there's something in here
          {
            _set_offsets[i][j] = SetOffsetToSet( _set_offsets[i][j] )->GetPrimaryOffset();
          }
        }
      }
      
      _released_set_offsets.clear(); // clear this.
      
      for ( int k = 1; k < SETS_SIZE; ++k )
      {
        if ( _sets[ k ] != NULL ) // something in this slot
        {
          SetHandle lSet = *_sets[k];
          
          if ( k != lSet->GetPrimaryOffset() )
          {
            delete _sets[k];
            _sets[k] = NULL;
            _released_set_offsets.push_back( k ); // it's null! hey!
          }
          else // we'll only hit each set once this way.
          {
            lSet->BackReferences.clear();
            lSet->BackReferences.push_back( lSet->Self ); // readd the canonical one.
          }
        }
        else
        {
          _released_set_offsets.push_back( k ); // it's null! hey!
        }
      }
      
      cout << "released " << _released_set_offsets.size() << " sets." << endl;
    }
    
    // add a hash to an existing set
    void add_to_set( SetHandle aSet, HashIntoType aHash )
    {      
      for ( int i = 0; i < HASHES; ++i ) // go through and make this hash and its bins and set offsets point at this set
      {
        SetOffsetBin lBin = HashBinToSetOffsetBinCached( HashToHashBinCached(aHash, i), i );    
        _set_offsets[i][ lBin ] = aSet->GetPrimaryOffset();        
        assert ( _set_offsets[i][ lBin ] > 0 ); // somehow we fucked this up.
      }
      
      aSet->Increment();
    }
    
    //
    // memory allocation functions
    //
    void populate_hash_table_bit_count_lookups()
    {
      cout << "populate_hash_table_bit_count_lookups" << endl;
      for (int i = 0; i < HASHES; ++i )
      {
        _hash_table_total_bit_counts[i] = _hash_table[i]->CountBits();
        
        unsigned long long lLookupTableSize = (_tablesizes[i] / BIT_COUNT_PARTITION)+1;
        for (unsigned long long j = 0; j < lLookupTableSize; ++j)
        {      
          unsigned long long lSectionStartIndex = j * BIT_COUNT_PARTITION;
          unsigned long long lSectionStopIndex = ((j + 1) * BIT_COUNT_PARTITION) - 1;
          if ( lSectionStopIndex >= _tablesizes[i] )
            lSectionStopIndex = _tablesizes[i] - 1;
          
          // temporarily store the single section count. (this section may be problematic, since ..lookup[i]'s value is a pointer, so what does [j] do?
          _hash_table_bit_counts_lookup[i][j] = _hash_table[i]->CountBits(lSectionStartIndex, lSectionStopIndex);
          
          if ( j > 0 ) // apply the summation
          {
            _hash_table_bit_counts_lookup[i][j] += _hash_table_bit_counts_lookup[i][j-1];
          }
        }
        cout << i << ": " << _hash_table_total_bit_counts[i] << " -- " << ((double)_hash_table_total_bit_counts[i] / (double)_tablesizes[i]) * 100 << "% occupancy" << endl;
      }
    }
    
    void deallocate_hash_table_preliminary()
    {
      for (int i = 0; i < HASHES; ++i)
      {
        delete _hash_table_preliminary[i];
      }      
    }
    
    void allocate_set_offset_table()
    {
      for (int i = 0; i < HASHES; ++i)
      {
        _set_offsets[i] = new unsigned short[ _hash_table_total_bit_counts[i] ];
        memset(_set_offsets[i], 0, _hash_table_total_bit_counts[i] * sizeof(unsigned short));
      }
    }
    
    // add every new set to the least crowded available set we can find.
    SetHandle create_set() // the basic starter set, everyone starts out fostered.
    {
      SetOffset lAddress = get_free_address();
      
      SetHandle lSet = NULL;
      
      if ( lAddress == 0 )
        lSet = get_least_crowded_set(); // fuck fuck fuck
      else
      {
        lSet = new CanonicalSet( lAddress );
        //_sorted_sets.push_back( lSet->Self );
        _sets[ lAddress ] = lSet->Self;
      }
      
      return lSet;
    }  
    
    void join ( SetHandle aJoinee, SetHandle aJoiner )
    {
      for ( int i = 0; i < aJoiner->BackReferences.size(); ++i )
      {
        *(aJoiner->BackReferences[i]) = aJoinee;
        aJoinee->BackReferences.push_back( aJoiner->BackReferences[i] );
      }
      
      aJoinee->Increment( aJoiner->GetKmerCount() );
      
      delete aJoiner;
    }
 
    SetHandle get_least_crowded_set()
    { 
      if (_sorted_sets.empty() )
        re_sort_sets();
      
      SetHandle lSmallestSet = *(_sorted_sets.back());
      _sorted_sets.pop_back();
      
      return lSmallestSet;
    }
    
    void re_sort_sets()
    {
      canonicalize(); // slim things out first.
      
      _sorted_sets.clear();
      for (int i = 0; i < SETS_SIZE; ++i )
      {
        if ( _sets[i] != NULL )
          _sorted_sets.push_back( _sets[i] );
      }
      
      sort( _sorted_sets.begin(), _sorted_sets.end(), CanonicalSet::CompSet() );
    }

    SetOffset get_free_address()
    {
      if ( !_released_set_offsets.empty() ) // we've got some released ones to go with.
        return get_a_released_offset(); 
      else if ( _last_set_offset < SETS_SIZE ) // no released ones, but we still have room at the head of the list
      {
        return ++_last_set_offset;
      }
      else
      {
//        canonicalize(); // clean shit out.
//        
//        if ( !_released_set_offsets.empty() ) // we've got some released ones to go with.
//          return get_a_released_offset(); 
//        else 
          return 0; // we're fucked. gotta start joining sets
      }
    }
    SetOffset get_a_released_offset()
    {
      SetOffset lSet = _released_set_offsets.back();
      _released_set_offsets.pop_back();
      
      return lSet;
    }
//    // create a real, honest to goodness addressed set.
//    SetHandle create_allocated_address_set()
//    {
//      SetHandle lSet = NULL;
//      
//      if ( !_released_set_offsets.empty() ) // we've got some released ones to go with.
//      {
//        lSet = new CanonicalSet( get_a_released_offset() ); 
////        add_to_fostering_elgibility( lSet );
////        _sets[ lSet->GetPrimaryGatewaySetOffset() ] = lSet;         
//      }
//      else if ( _last_set_offset < 65535 ) // no released ones, but we still have room at the head of the list
//      {
//        lSet = new CanonicalSet( ++_last_set_offset ); 
//        
////        add_to_fostering_elgibility( lSet );
//        _sets.push_back( lSet->Self );  
//      }
//      else
//      {
//        assert(0); // this is pretty fucked. how often does this happen?
//                   // in this case, you have to do some shady jiggery pokery.
//      }
//      
//      return lSet;
//    }

    
    
    
    //
    // helper functions
    //
    
    // calculate the hash bin from a kmer's hash
    // store the modulo output for future reference.
    HashBin HashToHashBinCached( HashIntoType aHash, int i ) // no idea if this will be faster than moduloing every time.
    {
      for (int j = 0, index = _hash_bin_cache_last_used_index[i]; j < CACHESIZE; ++j, ++index)
      {
        if ( index >= CACHESIZE )
          index = 0;
        
        if ( _hash_bin_cache[i][index].first == aHash )
        {
          _hash_bin_cache_last_used_index[i] = index;
          
//          HashBin lDELETEME = _hash_bin_cache[i][index].second;
          
          return _hash_bin_cache[i][index].second; // found it
        }
      }
      
      // didn't find it.
      HashBin lHashBin = HashToHashBin(aHash, i);
      
      // insert it into the cache;
      _hash_bin_cache_last_used_index[i]++;
      if ( _hash_bin_cache_last_used_index[i] >= CACHESIZE )
        _hash_bin_cache_last_used_index[i] = 0;
      
      _hash_bin_cache[i][ _hash_bin_cache_last_used_index[i] ] = pair<HashIntoType,HashBin>(aHash, lHashBin);
      
      return lHashBin;  
    }    
    HashBin HashToHashBin( HashIntoType aHash, int i ) // no idea which is faster.
    {
      return (aHash % _tablesizes[i]);
    }
    
    // calculate the set offset bin from a hash bin
    // store the value for future reference.
    SetOffsetBin HashBinToSetOffsetBinCached( HashBin aBin, int i ) // no idea if this will be faster
    {
      for (int j = 0, index = _set_offset_bin_cache_last_used_index[i]; j < CACHESIZE; ++j, ++index)
      {
        if ( index >= CACHESIZE )
          index = 0;
        
        if ( _set_offset_bin_cache[i][index].first == aBin )
        {
          _set_offset_bin_cache_last_used_index[i] = index;
          return _set_offset_bin_cache[i][index].second; // found it
        }
      }
      
      // didn't find it.
      HashBin lSetOffsetBin = HashBinToSetOffsetBin(aBin, i);
      
      // insert it into the cache;
      _set_offset_bin_cache_last_used_index[i]++;
      if ( _set_offset_bin_cache_last_used_index[i] >= CACHESIZE )
        _set_offset_bin_cache_last_used_index[i] = 0;
      
      _set_offset_bin_cache[i][ _set_offset_bin_cache_last_used_index[i] ] = pair<HashBin,SetOffsetBin>(aBin, lSetOffsetBin);
      
      return lSetOffsetBin;  
    }    
    SetOffsetBin HashBinToSetOffsetBin( HashBin aBin, int i )
    {      
      assert( aBin < _tablesizes[i] ); // make sure it's a valid bin.
      
      assert( _hash_table[i]->Get( aBin ) == true );
      
      unsigned long long lBinSectionIndex = (aBin / BIT_COUNT_PARTITION); // the index of the section before the one we're in
      assert( lBinSectionIndex <= (_tablesizes[i] / BIT_COUNT_PARTITION)); 
      
      unsigned long long lSetOffsetBin = _hash_table[i]->CountBits( lBinSectionIndex * BIT_COUNT_PARTITION, aBin );
      
      if ( lBinSectionIndex > 0 )      
        lSetOffsetBin += _hash_table_bit_counts_lookup[i][ lBinSectionIndex - 1 ];
      
      assert( lSetOffsetBin > 0 );
      
      lSetOffsetBin -= 1; // to make it an array index rather than a count.
      
      return lSetOffsetBin;
    }
    
    // find the set that a given hash goes with, based on its offset and hash
//    SetHandle SetOffsetAndHashToSet( SetOffset aOffset, HashIntoType aHash )
//    {      
//      assert( aOffset > 0 ); // we should not be poking here if we don't have a set to go to
//      
//      SetHandle lGatewayNode = *_sets[ aOffset ]; // pick up the gateway node
//      
//      return lGatewayNode;
//      
////      SetHandle lRoot = lGatewayNode->Root; // go to the top of the tree
////      
////      SetHandle lTargetSet = find_set_harboring_this_kmer( lRoot, aHash ); // start at the top
////      
////      if ( lTargetSet == NULL ) // means we're in the right place, but we've long since lost track of it. This is fine.
////        return lRoot; // the patriarch takes responsibility for it.
////      
////      return lTargetSet;      
//    }
    
    SetHandle SetOffsetToSet( SetOffset aOffset )
    {      
      assert( aOffset > 0 ); // we should not be poking here if we don't have a set to go to
      
      SetHandle lGatewayNode = *_sets[ aOffset ]; // pick up the gateway node
      
      return lGatewayNode;
      
      //      SetHandle lRoot = lGatewayNode->Root; // go to the top of the tree
      //      
      //      SetHandle lTargetSet = find_set_harboring_this_kmer( lRoot, aHash ); // start at the top
      //      
      //      if ( lTargetSet == NULL ) // means we're in the right place, but we've long since lost track of it. This is fine.
      //        return lRoot; // the patriarch takes responsibility for it.
      //      
      //      return lTargetSet;      
    }
    
    bool is_prime( unsigned long long aCandidate )
    {
      if ( aCandidate < 2 )
        return false;
      
      if ( aCandidate == 2 )
        return true;
      
      if ( aCandidate % 2 == 0 )
        return false;
      
      for ( unsigned long long i = 3; i < pow((double)aCandidate, 0.5) + 1; i+= 2 )
      {
        if ( aCandidate % i == 0 )
          return false;
      }
      
      return true;  
    }
    
    unsigned long long get_first_prime_below( unsigned long long aNumber )
    {
      unsigned long long i = aNumber - 1;
      
      if ( i % 2 == 0 ) // no even
        --i;
      
      while ( i > 0 )
      {
        if ( is_prime( i ) )
          return i;
        
        i -= 2;
      }
      
      return 0;
    }
    
    unsigned long long get_first_prime_above( unsigned long long aNumber )
    {
      unsigned long long i = aNumber + 1;
      
      if ( i % 2 == 0 ) // no even
        ++i;
      
      while ( true )
      {
        if ( is_prime( i ) )
          return i;
        
        i += 2;
      }
    }
  };
}
    