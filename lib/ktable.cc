#include <assert.h>
#include <math.h>
#include <string>
#include <iostream>
#include <algorithm>

#include "khmer.hh"
#include "ktable.hh"

using namespace std;
using namespace khmer;

//
// _hash: hash a k-length DNA sequence into a 64-bit number.
//

HashIntoType khmer::_hash(const char * kmer, const WordLength k, 
			  HashIntoType& _h, HashIntoType& _r)
{
  // sizeof(HashIntoType) * 8 bits / 2 bits/base  
  assert(k <= sizeof(HashIntoType)*4);

  HashIntoType h = 0, r = 0;

  h |= twobit_repr(kmer[0]);
  for (WordLength i = 1; i < k; i++) {
    h = h << 2;
    h |= twobit_repr(kmer[i]);
  }
  
  r = ~h; // bit complement
  
  // swap consecutive pairs
  r = ((r >> 2)  & 0x3333333333333333) | ((r & 0x3333333333333333) << 2);
  // swap nibbles 
  r = ((r >> 4)  & 0x0F0F0F0F0F0F0F0F) | ((r & 0x0F0F0F0F0F0F0F0F) << 4);
  // swap bytes
  r = ((r >> 8)  & 0x00FF00FF00FF00FF) | ((r & 0x00FF00FF00FF00FF) << 8);
  // swap 2-byte words
  r = ((r >> 16) & 0x0000FFFF0000FFFF) | ((r & 0x0000FFFF0000FFFF) << 16);
  // swap 2-word pairs
  r = ( r >> 32                      ) | ( r                       << 32);
  
  _h = h;
  _r = r;
  
  return uniqify_rc(h, r);
}



// _hash: return the maximum of the forward and reverse hash.

HashIntoType khmer::_hash(const char * kmer, const WordLength k)
{
  HashIntoType h = 0;
  HashIntoType r = 0;

  return _hash(kmer, k, h, r);
}

// _hash_forward: return the hash from the forward direction only.

HashIntoType khmer::_hash_forward(const char * kmer, WordLength k)
{
  HashIntoType h = 0;
  HashIntoType r = 0;

  
  _hash(kmer, k, h, r);
  return h;			// return forward only
}

//
// _revhash: given an unsigned int, return the associated k-mer.
//

std::string khmer::_revhash(HashIntoType hash, WordLength k)
{
  std::string s = "";

  unsigned int val = hash & 3;
  s += revtwobit_repr(val);

  for (WordLength i = 1; i < k; i++) {
    hash = hash >> 2;
    val = hash & 3;
    s += revtwobit_repr(val);
  }

  reverse(s.begin(), s.end());

  return s;
}

//
// consume_string: run through every k-mer in the given string, & hash it.
//

void KTable::consume_string(const std::string &s)
{
  const char * sp = s.c_str();
  unsigned int length = s.length();

#if 0
  const unsigned int length = s.length() - _ksize + 1;
  for (unsigned int i = 0; i < length; i++) {
    count(&sp[i]);
  }
#else

  unsigned long long int mask = 0;
  for (unsigned int i = 0; i < _ksize; i++) {
    mask = mask << 2;
    mask |= 3;
  }

  HashIntoType h;
  HashIntoType r;

  _hash(sp, _ksize, h, r);
  
  _counts[uniqify_rc(h, r)]++;

  for (unsigned int i = _ksize; i < length; i++) {
    // left-shift the previous hash over
    h = h << 2;

    // 'or' in the current nt
    h |= twobit_repr(sp[i]);

    // mask off the 2 bits we shifted over.
    h &= mask;

    // now handle reverse complement
    r = r >> 2;
    r |= (twobit_comp(sp[i]) << (_ksize*2 - 2));

    _counts[uniqify_rc(h, r)]++;
  }

#endif // 0
}

void KTable::update(const KTable &other)
{
  assert(_ksize == other._ksize);

  for (unsigned int i = 0; i < n_entries(); i++) {
    _counts[i] += other._counts[i];
  }
}

KTable * KTable::intersect(const KTable &other) const
{
  assert(_ksize == other._ksize);

  KTable * intersection = new KTable(_ksize);

  for (unsigned int i = 0; i < n_entries(); i++) {
    if (_counts[i] > 0 && other._counts[i] > 0) {
      intersection->_counts[i] = _counts[i] + other._counts[i];
    }
  }
  return intersection;
}
