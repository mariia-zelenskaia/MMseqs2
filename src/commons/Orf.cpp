/* Based on orf.cpp from NCBI C++ Toolkit
 * License:
 * $Id: orf.cpp 65735 2014-12-23 18:23:27Z astashya $
 * ===========================================================================
 *
 *                            PUBLIC DOMAIN NOTICE
 *               National Center for Biotechnology Information
 *
 *  This software/database is a "United States Government Work" under the
 *  terms of the United States Copyright Act.  It was written as part of
 *  the author's official duties as a United States Government employee and
 *  thus cannot be copyrighted.  This software/database is freely available
 *  to the public for use. The National Library of Medicine and the U.S.
 *  Government have not placed any restriction on its use or reproduction.
 *
 *  Although all reasonable efforts have been taken to ensure the accuracy
 *  and reliability of the software and data, the NLM and the U.S.
 *  Government do not and cannot warrant the performance or results that
 *  may be obtained by using this software or data. The NLM and the U.S.
 *  Government disclaim all warranties, express or implied, including
 *  warranties of performance, merchantability or fitness for any particular
 *  purpose.
 *
 *  Please cite the author in any work or product based on this material.
 *
 * ===========================================================================
 *
 * Authors:  Mike DiCuccio
 *
 * File Description:
 *
 */

#include "Orf.h"
#include "Util.h"
#include "Debug.h"
#include "TranslateNucl.h"
#include "simd.h"

#include <climits>
#include <cstring>
#include <cassert>
#include <cstdlib>
#include <algorithm>

//note: N->N, S->S, W->W, U->A, T->A
static const char* iupacReverseComplementTable =
"................................................................"
".TVGH..CD..M.KN...YSAABW.R.......tvgh..cd..m.kn...ysaabw.r......"
"................................................................"
"................................................................";

inline char complement(const char c) {
    return iupacReverseComplementTable[static_cast<unsigned char>(c)];
}

Orf::Orf(const unsigned int requestedGenCode, bool useAllTableStarts) {
    TranslateNucl translateNucl(static_cast<TranslateNucl::GenCode>(requestedGenCode));
    std::vector<std::string> codons = translateNucl.getStopCodons();
    stopCodons = (char*)mem_align(ALIGN_INT, 8 * sizeof(int));
    memset(stopCodons, 0, 8 * sizeof(int));
    size_t count = 0;
    for (size_t i = 0; i < codons.size(); ++i) {
        memcpy(stopCodons + count, codons[i].c_str(), 3);
        count += 4;
    }
    stopCodonCount = codons.size();
    if(stopCodonCount > 8) {
        Debug(Debug::ERROR) << "Invalid translation table with more than 8 stop codons.\n";
        EXIT(EXIT_FAILURE);
    }

    codons.clear();
    if(useAllTableStarts) {
        // if useAllTableStarts we take all alternatives for start codons from the table
        codons = translateNucl.getStartCodons();
    } else {
        codons.push_back("ATG");
    }

    startCodons = (char*)mem_align(ALIGN_INT, 8 * sizeof(int));
    memset(startCodons, 0, 8 * sizeof(int));
    count = 0;
    for (size_t i = 0; i < codons.size(); ++i) {
        memcpy(startCodons + count, codons[i].c_str(), 3);
        count += 4;
    }
    startCodonCount = codons.size();
    if(startCodonCount > 8) {
        Debug(Debug::ERROR) << "Invalid translation table with more than 8 start codons.\n";
        EXIT(EXIT_FAILURE);
    }

    sequence = (char*)mem_align(ALIGN_INT, 32000 * sizeof(char));
    reverseComplement = (char*)mem_align(ALIGN_INT, 32000 * sizeof(char));
    bufferSize = 32000;
}

Orf::~Orf() {
    free(sequence);
    free(reverseComplement);
    free(startCodons);
    free(stopCodons);
}

bool Orf::setSequence(const char* seq, size_t length) {
    if(length < 3) {
        return false;
    }

    if((length + VECSIZE_INT) > bufferSize) {
        free(sequence);
        free(reverseComplement);
        sequence = (char*)mem_align(ALIGN_INT, (length + VECSIZE_INT) * sizeof(char));
        reverseComplement = (char*)mem_align(ALIGN_INT, (length + VECSIZE_INT) * sizeof(char));
        bufferSize = (length + VECSIZE_INT);
    }

    sequenceLength = length;
    for(size_t i = 0; i < sequenceLength; ++i) {
        sequence[i] = seq[i] & ~0x20;
        if(sequence[i] == 'U') {
            sequence[i] = 'T';
        }
    }

    for(size_t i = 0; i < sequenceLength; ++i) {
        reverseComplement[i] = complement(sequence[sequenceLength - i - 1]);
        if(reverseComplement[i] == '.') {
            return false;
        }
    }

    for (size_t i = sequenceLength; i < sequenceLength + VECSIZE_INT; ++i) {
        sequence[i] = CHAR_MAX;
        reverseComplement[i] = CHAR_MAX;
    }

    return true;
}

std::pair<const char *, size_t> Orf::getSequence(const SequenceLocation &location) {
    assert(location.to > location.from);
    
    size_t length = location.to - location.from;
    if(location.strand == Orf::STRAND_PLUS) {
        return sequence ? std::make_pair(sequence + location.from, length) : std::make_pair("", 0);
    } else {
        return reverseComplement ? std::make_pair(reverseComplement + location.from, length) : std::make_pair("", 0);
    }
}

void Orf::findAll(std::vector<Orf::SequenceLocation> &result,
                  const size_t minLength,
                  const size_t maxLength,
                  const size_t maxGaps,
                  const unsigned int forwardFrames,
                  const unsigned int reverseFrames,
                  const unsigned int startMode) {
    if(forwardFrames != 0) {
        // find ORFs on the forward sequence
        findForward(sequence, sequenceLength, result,
                    minLength, maxLength, maxGaps, forwardFrames, startMode, STRAND_PLUS);
    }

    if(reverseFrames != 0) {
        // find ORFs on the reverse complement
        findForward(reverseComplement, sequenceLength, result,
                    minLength, maxLength, maxGaps, reverseFrames, startMode, STRAND_MINUS);
    }
}

inline bool isIncomplete(const char* codon) {
    return codon[0] == CHAR_MAX || codon[1] == CHAR_MAX || codon[2] == CHAR_MAX;
}

inline bool isGapOrN(const char *codon) {
    return codon[0] == 'N' || complement(codon[0]) == '.'
        || codon[1] == 'N' || complement(codon[1]) == '.'
        || codon[2] == 'N' || complement(codon[2]) == '.';
}

template <int N>
#ifndef AVX2
inline bool isInCodons(const char* sequence, simd_int codons, simd_int codons2) {
#else
inline bool isInCodons(const char* sequence, simd_int codons, simd_int) {
#endif
    simd_int c = simdi_loadu((simd_int*)sequence);
    // ATGA ATGA ATGA ATGA
#ifdef AVX2
    simd_int shuf = _mm256_permutevar8x32_epi32(c, _mm256_setzero_si256());
#else
    simd_int shuf = simdi32_shuffle(c, _MM_SHUFFLE(0, 0, 0, 0));
#endif
    // ATG0 ATG0 ATG0 ATG0
    simd_int mask = simdi32_set(0x00FFFFFF);
    shuf = simdi_and(mask, shuf);
    // FFFF 0000 0000 0000
    simd_int test = simdi32_eq(shuf, codons);
    #ifndef AVX2
    if(N > 4) {
        simd_int test2 = simdi32_eq(shuf, codons2);
        return (simdi8_movemask(test) != 0) && (simdi8_movemask(test2) != 0);
    }
    #endif
    return simdi8_movemask(test) != 0;
}

void Orf::findForward(const char *sequence, const size_t sequenceLength, std::vector<SequenceLocation> &result,
                      const size_t minLength, const size_t maxLength, const size_t maxGaps, const unsigned int frames,
                      const unsigned int startMode, const Strand strand) {
    // An open reading frame can beginning in any of the three codon start position
    // Frame 0:  AGA ATT GCC TGA ATA AAA GGA TTA CCT TGA TAG GGT AAA
    // Frame 1: A GAA TTG CCT GAA TAA AAG GAT TAC CTT GAT AGG GTA AA
    // Frame 2: AG AAT TGC CTG AAT AAA AGG ATT ACC TTG ATA GGG TAA A
    const int FRAMES = 3;
    const int frameLookup[FRAMES] = {FRAME_1, FRAME_2, FRAME_3};
    const size_t frameOffset[FRAMES] = {0, 1, 2};

    // We want to walk over the memory only once so we calculate which codon we are in
    // and save the values of our state machine in these arrays

    // we also initialize our state machine with being inside an orf
    // this is to handle edge case 1 where we find an end codon but no start codon
    // in this case we just add an orf from the start to the found end codon
    bool isInsideOrf[FRAMES]     = {true,  true,  true };
    bool hasStartCodon[FRAMES]   = {false, false, false};

    size_t countGaps[FRAMES]   = {0, 0, 0};
    size_t countLength[FRAMES] = {0, 0, 0};

    // Offset the start position by reading frame
    size_t from[FRAMES] = {frameOffset[0], frameOffset[1], frameOffset[2]};

    simd_int startCodonsHi = simdi_load((simd_int*)startCodons);
    simd_int startCodonsLo = simdi_loadu((simd_int*)(startCodons + 16));

    simd_int stopCodonsHi = simdi_load((simd_int*)stopCodons);
    simd_int stopCodonsLo = simdi_loadu((simd_int*)(stopCodons + 16));

    for (size_t i = 0;  i < sequenceLength - (FRAMES - 1);  i += FRAMES) {
        for(size_t position = i; position < i + FRAMES; position++) {
            const char* codon = sequence + position;
            size_t frame = position % FRAMES;

            // skip frames outside of out the frame mask
            if(!(frames & frameLookup[frame])) {
                continue;
            }

            bool thisIncomplete = isIncomplete(codon);
            bool isLast = !thisIncomplete && isIncomplete(codon + FRAMES);

            // START_TO_STOP returns the longest fragment such that the first codon is a start
            // ANY_TO_STOP returns the longest fragment
            // LAST_START_TO_STOP retruns last encountered start to stop,
            // no start codons in the middle
           
            bool shouldStart;
            if((startMode == START_TO_STOP)) {
                shouldStart = isInsideOrf[frame] == false 
                            && ((startCodonCount > 4) ? isInCodons<8>(codon, startCodonsHi, startCodonsLo)
                                                      : isInCodons<4>(codon, startCodonsHi, startCodonsLo));
            } else if(startMode == ANY_TO_STOP) {
                shouldStart = isInsideOrf[frame] == false;
            } else {
                // LAST_START_TO_STOP:
                shouldStart = ((startCodonCount > 4) ? isInCodons<8>(codon, startCodonsHi, startCodonsLo)
                                                     : isInCodons<4>(codon, startCodonsHi, startCodonsLo));
            }

            // do not start a new orf on the last codon
            if(shouldStart && isLast == false) {
                isInsideOrf[frame] = true;
                hasStartCodon[frame] = true;
                from[frame] = position;

                countGaps[frame] = 0;
                countLength[frame] = 0;
            }

            if(isInsideOrf[frame]) {
                countLength[frame]++;

                if(isGapOrN(codon)) {
                    countGaps[frame]++;
                }
            }

            const bool stop = (stopCodonCount > 4) ? isInCodons<8>(codon, stopCodonsHi, stopCodonsLo)
                                                   : isInCodons<4>(codon, stopCodonsHi, stopCodonsLo);
            if(isInsideOrf[frame] && (stop || isLast)) {
                isInsideOrf[frame] = false;

                // we include the stop codon here
                size_t to = position + (isLast ? 3 : 0);

                // this could happen if the first codon is a stop codon
                if(to == from[frame])
                    continue;

                assert(to > from[frame]);

                // ignore orfs with too many gaps or unknown codons
                // also ignore orfs shorter than the min size and longer than max
                if ((countGaps[frame] > maxGaps)
                || (countLength[frame] > maxLength)
                || (countLength[frame] <= minLength)) {
                    continue;
                }

                result.emplace_back(from[frame], to, !hasStartCodon[frame], !stop, strand);
            }
        }
    }
}

Orf::SequenceLocation Orf::parseOrfHeader(char *data) {
    char * entry[255];
    size_t columns = Util::getWordsOfLine(data, entry, 255);
    size_t col;
    bool found = false;
    for(col = 0; col < columns; col++){
        if(entry[col][0] == '[' && entry[col][1] == 'O' && entry[col][2] == 'r' && entry[col][3] == 'f' && entry[col][4] == ':'){
            found=true;
            break;
        }
    }
    if(found == false){
        Debug(Debug::ERROR) << "Could not find Orf information in header.\n";
        EXIT(EXIT_FAILURE);
    }
    Orf::SequenceLocation loc;
    int strand;
    int hasIncompleteStart, hasIncompleteEnd;
    int retCode = sscanf(entry[col], "[Orf: %u, %zu, %zu, %d, %d, %d]", &loc.id, &loc.from, &loc.to, &strand, &hasIncompleteStart, &hasIncompleteEnd);
    loc.hasIncompleteStart = hasIncompleteStart;
    loc.hasIncompleteEnd = hasIncompleteEnd;
    if(retCode < 5) {
        Debug(Debug::ERROR) << "Could not parse Orf " << entry[col] << ".\n";
        EXIT(EXIT_FAILURE);
    }
    loc.strand =  static_cast<Orf::Strand>(strand);
    return loc;
}
