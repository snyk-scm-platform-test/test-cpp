/*-----------------------------------------------------------*/
/*--- Block recoverer program for bzip2                   ---*/
/*---                                      bzip2recover.c ---*/
/*-----------------------------------------------------------*/

/* ------------------------------------------------------------------
   This file is part of bzip2/libbzip2, a program and library for
   lossless, block-sorting data compression.

   bzip2/libbzip2 version 1.0.8 of 13 July 2019
   Copyright (C) 1996-2019 Julian Seward <jseward@acm.org>

   Please read the WARNING, DISCLAIMER and PATENTS sections in the 
   README file.

   This program is released under the terms of the license contained
   in the file LICENSE.
   ------------------------------------------------------------------ */

/* This program is a complete hack and should be rewritten properly.
	 It isn't very complicated. */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>


/* This program records bit locations in the file to be recovered.
   That means that if 64-bit ints are not supported, we will not
   be able to recover .bz2 files over 512MB (2^32 bits) long.
   On GNU supported platforms, we take advantage of the 64-bit
   int support to circumvent this problem.  Ditto MSVC.

   This change occurred in version 1.0.2; all prior versions have
   the 512MB limitation.
*/
#ifdef __GNUC__
   typedef  unsigned long long int  MaybeUInt64;
#  define MaybeUInt64_FMT "%Lu"
#else
#ifdef _MSC_VER
   typedef  unsigned __int64  MaybeUInt64;
#  define MaybeUInt64_FMT "%I64u"
#else
   typedef  unsigned int   MaybeUInt64;
#  define MaybeUInt64_FMT "%u"
#endif
#endif

typedef  unsigned int   UInt32;
typedef  int            Int32;
typedef  unsigned char  UChar;
typedef  char           Char;
typedef  unsigned char  Bool;
#define True    ((Bool)1)
#define False   ((Bool)0)


#define BZ_MAX_FILENAME 2000

Char inFileName[BZ_MAX_FILENAME];
Char outFileName[BZ_MAX_FILENAME];
Char progName[BZ_MAX_FILENAME];

MaybeUInt64 bytesOut = 0;
MaybeUInt64 bytesIn  = 0;


/*---------------------------------------------------*/
/*--- Header bytes                                ---*/
/*---------------------------------------------------*/

#define BZ_HDR_B 0x42                         /* 'B' */
#define BZ_HDR_Z 0x5a                         /* 'Z' */
#define BZ_HDR_h 0x68                         /* 'h' */
#define BZ_HDR_0 0x30                         /* '0' */
 

/*---------------------------------------------------*/
/*--- I/O errors                                  ---*/
/*---------------------------------------------------*/

/*---------------------------------------------*/
static void readError ( void )
{
   fprintf ( stderr,
             "%s: I/O error reading `%s', possible reason follows.\n",
            progName, inFileName );
   perror ( progName );
   fprintf ( stderr, "%s: warning: output file(s) may be incomplete.\n",
             progName );
   exit ( 1 );
}


/*---------------------------------------------*/
static void writeError ( void )
{
   fprintf ( stderr,
             "%s: I/O error reading `%s', possible reason follows.\n",
            progName, inFileName );
   perror ( progName );
   fprintf ( stderr, "%s: warning: output file(s) may be incomplete.\n",
             progName );
   exit ( 1 );
}


/*---------------------------------------------*/
static void mallocFail ( Int32 n )
{
   fprintf ( stderr,
             "%s: malloc failed on request for %d bytes.\n",
            progName, n );
   fprintf ( stderr, "%s: warning: output file(s) may be incomplete.\n",
             progName );
   exit ( 1 );
}


/*---------------------------------------------*/
static void tooManyBlocks ( Int32 max_handled_blocks )
{
   fprintf ( stderr,
             "%s: `%s' appears to contain more than %d blocks\n",
            progName, inFileName, max_handled_blocks );
   fprintf ( stderr,
             "%s: and cannot be handled.  To fix, increase\n",
             progName );
   fprintf ( stderr, 
             "%s: BZ_MAX_HANDLED_BLOCKS in bzip2recover.c, and recompile.\n",
             progName );
   exit ( 1 );
}



/*---------------------------------------------------*/
/*--- Bit stream I/O                              ---*/
/*---------------------------------------------------*/

typedef
   struct {
      FILE*  handle;
      Int32  buffer;
      Int32  buffLive;
      Char   mode;
   }
   BitStream;


/*---------------------------------------------*/
static BitStream* bsOpenReadStream ( FILE* stream )
{
   BitStream *bs = malloc ( sizeof(BitStream) );
   if (bs == NULL) mallocFail ( sizeof(BitStream) );
   bs->handle = stream;
   bs->buffer = 0;
   bs->buffLive = 0;
   bs->mode = 'r';
   return bs;
}


/*---------------------------------------------*/
static BitStream* bsOpenWriteStream ( FILE* stream )
{
   BitStream *bs = malloc ( sizeof(BitStream) );
   if (bs == NULL) mallocFail ( sizeof(BitStream) );
   bs->handle = stream;
   bs->buffer = 0;
   bs->buffLive = 0;
   bs->mode = 'w';
   return bs;
}


/*---------------------------------------------*/
static void bsPutBit ( BitStream* bs, Int32 bit )
{
   if (bs->buffLive == 8) {
      Int32 retVal = putc ( (UChar) bs->buffer, bs->handle );
      if (retVal == EOF) writeError();
      bytesOut++;
      bs->buffLive = 1;
      bs->buffer = bit & 0x1;
   } else {
      bs->buffer = ( (bs->buffer << 1) | (bit & 0x1) );
      bs->buffLive++;
   };
}


/*---------------------------------------------*/
/*--
   Returns 0 or 1, or 2 to indicate EOF.
--*/
static Int32 bsGetBit ( BitStream* bs )
{
   if (bs->buffLive > 0) {
      bs->buffLive --;
      return ( ((bs->buffer) >> (bs->buffLive)) & 0x1 );
   } else {
      Int32 retVal = getc ( bs->handle );
      if ( retVal == EOF ) {
         if (errno != 0) readError();
         return 2;
      }
      bs->buffLive = 7;
      bs->buffer = retVal;
      return ( ((bs->buffer) >> 7) & 0x1 );
   }
}


/*---------------------------------------------*/
static void bsClose ( BitStream* bs )
{
   Int32 retVal;

   if ( bs->mode == 'w' ) {
      while ( bs->buffLive < 8 ) {
         bs->buffLive++;
         bs->buffer <<= 1;
      };
      retVal = putc ( (UChar) (bs->buffer), bs->handle );
      if (retVal == EOF) writeError();
      bytesOut++;
      retVal = fflush ( bs->handle );
      if (retVal == EOF) writeError();
   }
   retVal = fclose ( bs->handle );
   if (retVal == EOF) {
      if (bs->mode == 'w') writeError(); else readError();
   }
   free ( bs );
}


/*---------------------------------------------*/
static void bsPutUChar ( BitStream* bs, UChar c )
{
   Int32 i;
   for (i = 7; i >= 0; i--)
      bsPutBit ( bs, (((UInt32) c) >> i) & 0x1 );
}


/*---------------------------------------------*/
static void bsPutUInt32 ( BitStream* bs, UInt32 c )
{
   Int32 i;

   for (i = 31; i >= 0; i--)
      bsPutBit ( bs, (c >> i) & 0x1 );
}


/*---------------------------------------------*/
static Bool endsInBz2 ( Char* name )
{
   Int32 n = strlen ( name );
   if (n <= 4) return False;
   return
      (name[n-4] == '.' &&
       name[n-3] == 'b' &&
       name[n-2] == 'z' &&
       name[n-1] == '2');
}


/*---------------------------------------------------*/
/*---                                             ---*/
/*---------------------------------------------------*/

/* This logic isn't really right when it comes to Cygwin. */
#ifdef _WIN32
#  define  BZ_SPLIT_SYM  '\\'  /* path splitter on Windows platform */
#else
#  define  BZ_SPLIT_SYM  '/'   /* path splitter on Unix platform */
#endif

#define BLOCK_HEADER_HI  0x00003141UL
#define BLOCK_HEADER_LO  0x59265359UL

#define BLOCK_ENDMARK_HI 0x00001772UL
#define BLOCK_ENDMARK_LO 0x45385090UL

/* Increase if necessary.  However, a .bz2 file with > 50000 blocks
   would have an uncompressed size of at least 40GB, so the chances
   are low you'll need to up this.
*/
#define BZ_MAX_HANDLED_BLOCKS 50000

MaybeUInt64 bStart [BZ_MAX_HANDLED_BLOCKS];
MaybeUInt64 bEnd   [BZ_MAX_HANDLED_BLOCKS];
MaybeUInt64 rbStart[BZ_MAX_HANDLED_BLOCKS];
MaybeUInt64 rbEnd  [BZ_MAX_HANDLED_BLOCKS];

Int32 main ( Int32 argc, Char** argv )
{
   FILE*       inFile;
   FILE*       outFile;
   BitStream*  bsIn, *bsWr;
   Int32       b, wrBlock, currBlock, rbCtr;
   MaybeUInt64 bitsRead;

   UInt32      buffHi, buffLo, blockCRC;
   Char*       p;

   strncpy ( progName, argv[0], BZ_MAX_FILENAME-1);
   progName[BZ_MAX_FILENAME-1]='\0';
   inFileName[0] = outFileName[0] = 0;

   fprintf ( stderr, 
             "bzip2recover 1.0.8: extracts blocks from damaged .bz2 files.\n" );

   if (argc != 2) {
      fprintf ( stderr, "%s: usage is `%s damaged_file_name'.\n",
                        progName, progName );
      switch (sizeof(MaybeUInt64)) {
         case 8:
            fprintf(stderr, 
                    "\trestrictions on size of recovered file: None\n");
            break;
         case 4:
            fprintf(stderr, 
                    "\trestrictions on size of recovered file: 512 MB\n");
            fprintf(stderr, 
                    "\tto circumvent, recompile with MaybeUInt64 as an\n"
                    "\tunsigned 64-bit int.\n");
            break;
         default:
            fprintf(stderr, 
                    "\tsizeof(MaybeUInt64) is not 4 or 8 -- "
                    "configuration error.\n");
            break;
      }
      exit(1);
   }

   if (strlen(argv[1]) >= BZ_MAX_FILENAME-20) {
      fprintf ( stderr, 
                "%s: supplied filename is suspiciously (>= %d chars) long.  Bye!\n",
                progName, (int)strlen(argv[1]) );
      exit(1);
   }

   strcpy ( inFileName, argv[1] );

   inFile = fopen ( inFileName, "rb" );
   if (inFile == NULL) {
      fprintf ( stderr, "%s: can't read `%s'\n", progName, inFileName );
      exit(1);
   }

   bsIn = bsOpenReadStream ( inFile );
   fprintf ( stderr, "%s: searching for block boundaries ...\n", progName );

   bitsRead = 0;
   buffHi = buffLo = 0;
   currBlock = 0;
   bStart[currBlock] = 0;

   rbCtr = 0;

   while (True) {
      b = bsGetBit ( bsIn );
      bitsRead++;
      if (b == 2) {
         if (bitsRead >= bStart[currBlock] &&
            (bitsRead - bStart[currBlock]) >= 40) {
            bEnd[currBlock] = bitsRead-1;
            if (currBlock > 0)
               fprintf ( stderr, "   block %d runs from " MaybeUInt64_FMT 
                                 " to " MaybeUInt64_FMT " (incomplete)\n",
                         currBlock,  bStart[currBlock], bEnd[currBlock] );
         } else
            currBlock--;
         break;
      }
      buffHi = (buffHi << 1) | (buffLo >> 31);
      buffLo = (buffLo << 1) | (b & 1);
      if ( ( (buffHi & 0x0000ffff) == BLOCK_HEADER_HI 
             && buffLo == BLOCK_HEADER_LO)
           || 
           ( (buffHi & 0x0000ffff) == BLOCK_ENDMARK_HI 
             && buffLo == BLOCK_ENDMARK_LO)
         ) {
         if (bitsRead > 49) {
            bEnd[currBlock] = bitsRead-49;
         } else {
            bEnd[currBlock] = 0;
         }
         if (currBlock > 0 &&
	     (bEnd[currBlock] - bStart[currBlock]) >= 130) {
            fprintf ( stderr, "   block %d runs from " MaybeUInt64_FMT 
                              " to " MaybeUInt64_FMT "\n",
                      rbCtr+1,  bStart[currBlock], bEnd[currBlock] );
            rbStart[rbCtr] = bStart[currBlock];
            rbEnd[rbCtr] = bEnd[currBlock];
            rbCtr++;
         }
         if (currBlock >= BZ_MAX_HANDLED_BLOCKS)
            tooManyBlocks(BZ_MAX_HANDLED_BLOCKS);
         currBlock++;

         bStart[currBlock] = bitsRead;
      }
   }

   bsClose ( bsIn );

   /*-- identified blocks run from 1 to rbCtr inclusive. --*/

   if (rbCtr < 1) {
      fprintf ( stderr,
                "%s: sorry, I couldn't find any block boundaries.\n",
                progName );
      exit(1);
   };

   fprintf ( stderr, "%s: splitting into blocks\n", progName );

   inFile = fopen ( inFileName, "rb" );
   if (inFile == NULL) {
      fprintf ( stderr, "%s: can't open `%s'\n", progName, inFileName );
      exit(1);
   }
   bsIn = bsOpenReadStream ( inFile );

   /*-- placate gcc's dataflow analyser --*/
   blockCRC = 0; bsWr = 0;

   bitsRead = 0;
   outFile = NULL;
   wrBlock = 0;
   while (True) {
      b = bsGetBit(bsIn);
      if (b == 2) break;
      buffHi = (buffHi << 1) | (buffLo >> 31);
      buffLo = (buffLo << 1) | (b & 1);
      if (bitsRead == 47+rbStart[wrBlock]) 
         blockCRC = (buffHi << 16) | (buffLo >> 16);

      if (outFile != NULL && bitsRead >= rbStart[wrBlock]
                          && bitsRead <= rbEnd[wrBlock]) {
         bsPutBit ( bsWr, b );
      }

      bitsRead++;

      if (bitsRead == rbEnd[wrBlock]+1) {
         if (outFile != NULL) {
            bsPutUChar ( bsWr, 0x17 ); bsPutUChar ( bsWr, 0x72 );
            bsPutUChar ( bsWr, 0x45 ); bsPutUChar ( bsWr, 0x38 );
            bsPutUChar ( bsWr, 0x50 ); bsPutUChar ( bsWr, 0x90 );
            bsPutUInt32 ( bsWr, blockCRC );
            bsClose ( bsWr );
            outFile = NULL;
         }
         if (wrBlock >= rbCtr) break;
         wrBlock++;
      } else
      if (bitsRead == rbStart[wrBlock]) {
         /* Create the output file name, correctly handling leading paths. 
            (31.10.2001 by Sergey E. Kusikov) */
         Char* split;
         Int32 ofs, k;
         for (k = 0; k < BZ_MAX_FILENAME; k++) 
            outFileName[k] = 0;
         strcpy (outFileName, inFileName);
         split = strrchr (outFileName, BZ_SPLIT_SYM);
         if (split == NULL) {
            split = outFileName;
         } else {
            ++split;
	 }
	 /* Now split points to the start of the basename. */
         ofs  = split - outFileName;
         sprintf (split, "rec%5d", wrBlock+1);
         for (p = split; *p != 0; p++) if (*p == ' ') *p = '0';
         strcat (outFileName, inFileName + ofs);

         if ( !endsInBz2(outFileName)) strcat ( outFileName, ".bz2" );

         fprintf ( stderr, "   writing block %d to `%s' ...\n",
                           wrBlock+1, outFileName );

         outFile = fopen ( outFileName, "wb" );
         if (outFile == NULL) {
            fprintf ( stderr, "%s: can't write `%s'\n",
                      progName, outFileName );
            exit(1);
         }
         bsWr = bsOpenWriteStream ( outFile );
         bsPutUChar ( bsWr, BZ_HDR_B );    
         bsPutUChar ( bsWr, BZ_HDR_Z );    
         bsPutUChar ( bsWr, BZ_HDR_h );    
         bsPutUChar ( bsWr, BZ_HDR_0 + 9 );
         bsPutUChar ( bsWr, 0x31 ); bsPutUChar ( bsWr, 0x41 );
         bsPutUChar ( bsWr, 0x59 ); bsPutUChar ( bsWr, 0x26 );
         bsPutUChar ( bsWr, 0x53 ); bsPutUChar ( bsWr, 0x59 );
      }
   }

   fprintf ( stderr, "%s: finished\n", progName );
   return 0;
}



/*-----------------------------------------------------------*/
/*--- end                                  bzip2recover.c ---*/
/*-----------------------------------------------------------*/


/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing#kwsys for details.  */
#include "kwsysPrivate.h"
#include KWSYS_HEADER(MD5.h)

/* Work-around CMake dependency scanning limitation.  This must
   duplicate the above list of headers.  */
#if 0
#  include "MD5.h.in"
#endif

#include <stddef.h> /* size_t */
#include <stdint.h> /* uintptr_t */
#include <stdlib.h> /* malloc, free */
#include <string.h> /* memcpy, strlen */

/* This MD5 implementation has been taken from a third party.  Slight
   modifications to the arrangement of the code have been made to put
   it in a single source file instead of a separate header and
   implementation file.  */

#if defined(__clang__) && !defined(__INTEL_COMPILER)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wcast-align"
#endif

/*
  Copyright (C) 1999, 2000, 2002 Aladdin Enterprises.  All rights reserved.
  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.
  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:
  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
  L. Peter Deutsch
  ghost@aladdin.com
 */
/*
  Independent implementation of MD5 (RFC 1321).
  This code implements the MD5 Algorithm defined in RFC 1321, whose
  text is available at
        http://www.ietf.org/rfc/rfc1321.txt
  The code is derived from the text of the RFC, including the test suite
  (section A.5) but excluding the rest of Appendix A.  It does not include
  any code or documentation that is identified in the RFC as being
  copyrighted.
  The original and principal author of md5.c is L. Peter Deutsch
  <ghost@aladdin.com>.  Other authors are noted in the change history
  that follows (in reverse chronological order):
  2002-04-13 lpd Clarified derivation from RFC 1321; now handles byte order
        either statically or dynamically; added missing #include <string.h>
        in library.
  2002-03-11 lpd Corrected argument list for main(), and added int return
        type, in test program and T value program.
  2002-02-21 lpd Added missing #include <stdio.h> in test program.
  2000-07-03 lpd Patched to eliminate warnings about "constant is
        unsigned in ANSI C, signed in traditional"; made test program
        self-checking.
  1999-11-04 lpd Edited comments slightly for automatic TOC extraction.
  1999-10-18 lpd Fixed typo in header comment (ansi2knr rather than md5).
  1999-05-03 lpd Original version.
 */

/*
 * This package supports both compile-time and run-time determination of CPU
 * byte order.  If ARCH_IS_BIG_ENDIAN is defined as 0, the code will be
 * compiled to run only on little-endian CPUs; if ARCH_IS_BIG_ENDIAN is
 * defined as non-zero, the code will be compiled to run only on big-endian
 * CPUs; if ARCH_IS_BIG_ENDIAN is not defined, the code will be compiled to
 * run on either big- or little-endian CPUs, but will run slightly less
 * efficiently on either one than if ARCH_IS_BIG_ENDIAN is defined.
 */

typedef unsigned char md5_byte_t; /* 8-bit byte */
typedef unsigned int md5_word_t;  /* 32-bit word */

/* Define the state of the MD5 Algorithm. */
typedef struct md5_state_s
{
  md5_word_t count[2]; /* message length in bits, lsw first */
  md5_word_t abcd[4];  /* digest buffer */
  md5_byte_t buf[64];  /* accumulate block */
} md5_state_t;

#undef BYTE_ORDER /* 1 = big-endian, -1 = little-endian, 0 = unknown */
#ifdef ARCH_IS_BIG_ENDIAN
#  define BYTE_ORDER (ARCH_IS_BIG_ENDIAN ? 1 : -1)
#else
#  define BYTE_ORDER 0
#endif

#define T_MASK ((md5_word_t)~0)
#define T1 /* 0xd76aa478 */ (T_MASK ^ 0x28955b87)
#define T2 /* 0xe8c7b756 */ (T_MASK ^ 0x173848a9)
#define T3 0x242070db
#define T4 /* 0xc1bdceee */ (T_MASK ^ 0x3e423111)
#define T5 /* 0xf57c0faf */ (T_MASK ^ 0x0a83f050)
#define T6 0x4787c62a
#define T7 /* 0xa8304613 */ (T_MASK ^ 0x57cfb9ec)
#define T8 /* 0xfd469501 */ (T_MASK ^ 0x02b96afe)
#define T9 0x698098d8
#define T10 /* 0x8b44f7af */ (T_MASK ^ 0x74bb0850)
#define T11 /* 0xffff5bb1 */ (T_MASK ^ 0x0000a44e)
#define T12 /* 0x895cd7be */ (T_MASK ^ 0x76a32841)
#define T13 0x6b901122
#define T14 /* 0xfd987193 */ (T_MASK ^ 0x02678e6c)
#define T15 /* 0xa679438e */ (T_MASK ^ 0x5986bc71)
#define T16 0x49b40821
#define T17 /* 0xf61e2562 */ (T_MASK ^ 0x09e1da9d)
#define T18 /* 0xc040b340 */ (T_MASK ^ 0x3fbf4cbf)
#define T19 0x265e5a51
#define T20 /* 0xe9b6c7aa */ (T_MASK ^ 0x16493855)
#define T21 /* 0xd62f105d */ (T_MASK ^ 0x29d0efa2)
#define T22 0x02441453
#define T23 /* 0xd8a1e681 */ (T_MASK ^ 0x275e197e)
#define T24 /* 0xe7d3fbc8 */ (T_MASK ^ 0x182c0437)
#define T25 0x21e1cde6
#define T26 /* 0xc33707d6 */ (T_MASK ^ 0x3cc8f829)
#define T27 /* 0xf4d50d87 */ (T_MASK ^ 0x0b2af278)
#define T28 0x455a14ed
#define T29 /* 0xa9e3e905 */ (T_MASK ^ 0x561c16fa)
#define T30 /* 0xfcefa3f8 */ (T_MASK ^ 0x03105c07)
#define T31 0x676f02d9
#define T32 /* 0x8d2a4c8a */ (T_MASK ^ 0x72d5b375)
#define T33 /* 0xfffa3942 */ (T_MASK ^ 0x0005c6bd)
#define T34 /* 0x8771f681 */ (T_MASK ^ 0x788e097e)
#define T35 0x6d9d6122
#define T36 /* 0xfde5380c */ (T_MASK ^ 0x021ac7f3)
#define T37 /* 0xa4beea44 */ (T_MASK ^ 0x5b4115bb)
#define T38 0x4bdecfa9
#define T39 /* 0xf6bb4b60 */ (T_MASK ^ 0x0944b49f)
#define T40 /* 0xbebfbc70 */ (T_MASK ^ 0x4140438f)
#define T41 0x289b7ec6
#define T42 /* 0xeaa127fa */ (T_MASK ^ 0x155ed805)
#define T43 /* 0xd4ef3085 */ (T_MASK ^ 0x2b10cf7a)
#define T44 0x04881d05
#define T45 /* 0xd9d4d039 */ (T_MASK ^ 0x262b2fc6)
#define T46 /* 0xe6db99e5 */ (T_MASK ^ 0x1924661a)
#define T47 0x1fa27cf8
#define T48 /* 0xc4ac5665 */ (T_MASK ^ 0x3b53a99a)
#define T49 /* 0xf4292244 */ (T_MASK ^ 0x0bd6ddbb)
#define T50 0x432aff97
#define T51 /* 0xab9423a7 */ (T_MASK ^ 0x546bdc58)
#define T52 /* 0xfc93a039 */ (T_MASK ^ 0x036c5fc6)
#define T53 0x655b59c3
#define T54 /* 0x8f0ccc92 */ (T_MASK ^ 0x70f3336d)
#define T55 /* 0xffeff47d */ (T_MASK ^ 0x00100b82)
#define T56 /* 0x85845dd1 */ (T_MASK ^ 0x7a7ba22e)
#define T57 0x6fa87e4f
#define T58 /* 0xfe2ce6e0 */ (T_MASK ^ 0x01d3191f)
#define T59 /* 0xa3014314 */ (T_MASK ^ 0x5cfebceb)
#define T60 0x4e0811a1
#define T61 /* 0xf7537e82 */ (T_MASK ^ 0x08ac817d)
#define T62 /* 0xbd3af235 */ (T_MASK ^ 0x42c50dca)
#define T63 0x2ad7d2bb
#define T64 /* 0xeb86d391 */ (T_MASK ^ 0x14792c6e)

static void md5_process(md5_state_t* pms, const md5_byte_t* data /*[64]*/)
{
  md5_word_t a = pms->abcd[0];
  md5_word_t b = pms->abcd[1];
  md5_word_t c = pms->abcd[2];
  md5_word_t d = pms->abcd[3];
  md5_word_t t;
#if BYTE_ORDER > 0
  /* Define storage only for big-endian CPUs. */
  md5_word_t X[16];
#else
  /* Define storage for little-endian or both types of CPUs. */
  md5_word_t xbuf[16];
  const md5_word_t* X;
#endif

  {
#if BYTE_ORDER == 0
    /*
     * Determine dynamically whether this is a big-endian or
     * little-endian machine, since we can use a more efficient
     * algorithm on the latter.
     */
    static const int w = 1;

    if (*((const md5_byte_t*)&w)) /* dynamic little-endian */
#endif
#if BYTE_ORDER <= 0 /* little-endian */
    {
      /*
       * On little-endian machines, we can process properly aligned
       * data without copying it.
       */
      if (!((uintptr_t)data & 3)) {
        /* data are properly aligned */
        X = (const md5_word_t*)data;
      } else {
        /* not aligned */
        memcpy(xbuf, data, 64);
        X = xbuf;
      }
    }
#endif
#if BYTE_ORDER == 0
    else /* dynamic big-endian */
#endif
#if BYTE_ORDER >= 0 /* big-endian */
    {
      /*
       * On big-endian machines, we must arrange the bytes in the
       * right order.
       */
      const md5_byte_t* xp = data;
      int i;

#  if BYTE_ORDER == 0
      X = xbuf; /* (dynamic only) */
#  else
#    define xbuf X /* (static only) */
#  endif
      for (i = 0; i < 16; ++i, xp += 4) {
        xbuf[i] =
          (md5_word_t)(xp[0] + (xp[1] << 8) + (xp[2] << 16) + (xp[3] << 24));
      }
    }
#endif
  }

#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* Round 1. */
/* Let [abcd k s i] denote the operation
   a = b + ((a + F(b,c,d) + X[k] + T[i]) <<< s). */
#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define SET(a, b, c, d, k, s, Ti)                                             \
  t = a + F(b, c, d) + X[k] + (Ti);                                           \
  a = ROTATE_LEFT(t, s) + b
  /* Do the following 16 operations. */
  SET(a, b, c, d, 0, 7, T1);
  SET(d, a, b, c, 1, 12, T2);
  SET(c, d, a, b, 2, 17, T3);
  SET(b, c, d, a, 3, 22, T4);
  SET(a, b, c, d, 4, 7, T5);
  SET(d, a, b, c, 5, 12, T6);
  SET(c, d, a, b, 6, 17, T7);
  SET(b, c, d, a, 7, 22, T8);
  SET(a, b, c, d, 8, 7, T9);
  SET(d, a, b, c, 9, 12, T10);
  SET(c, d, a, b, 10, 17, T11);
  SET(b, c, d, a, 11, 22, T12);
  SET(a, b, c, d, 12, 7, T13);
  SET(d, a, b, c, 13, 12, T14);
  SET(c, d, a, b, 14, 17, T15);
  SET(b, c, d, a, 15, 22, T16);
#undef SET

/* Round 2. */
/* Let [abcd k s i] denote the operation
     a = b + ((a + G(b,c,d) + X[k] + T[i]) <<< s). */
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define SET(a, b, c, d, k, s, Ti)                                             \
  t = a + G(b, c, d) + X[k] + (Ti);                                           \
  a = ROTATE_LEFT(t, s) + b
  /* Do the following 16 operations. */
  SET(a, b, c, d, 1, 5, T17);
  SET(d, a, b, c, 6, 9, T18);
  SET(c, d, a, b, 11, 14, T19);
  SET(b, c, d, a, 0, 20, T20);
  SET(a, b, c, d, 5, 5, T21);
  SET(d, a, b, c, 10, 9, T22);
  SET(c, d, a, b, 15, 14, T23);
  SET(b, c, d, a, 4, 20, T24);
  SET(a, b, c, d, 9, 5, T25);
  SET(d, a, b, c, 14, 9, T26);
  SET(c, d, a, b, 3, 14, T27);
  SET(b, c, d, a, 8, 20, T28);
  SET(a, b, c, d, 13, 5, T29);
  SET(d, a, b, c, 2, 9, T30);
  SET(c, d, a, b, 7, 14, T31);
  SET(b, c, d, a, 12, 20, T32);
#undef SET

/* Round 3. */
/* Let [abcd k s t] denote the operation
     a = b + ((a + H(b,c,d) + X[k] + T[i]) <<< s). */
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define SET(a, b, c, d, k, s, Ti)                                             \
  t = a + H(b, c, d) + X[k] + (Ti);                                           \
  a = ROTATE_LEFT(t, s) + b
  /* Do the following 16 operations. */
  SET(a, b, c, d, 5, 4, T33);
  SET(d, a, b, c, 8, 11, T34);
  SET(c, d, a, b, 11, 16, T35);
  SET(b, c, d, a, 14, 23, T36);
  SET(a, b, c, d, 1, 4, T37);
  SET(d, a, b, c, 4, 11, T38);
  SET(c, d, a, b, 7, 16, T39);
  SET(b, c, d, a, 10, 23, T40);
  SET(a, b, c, d, 13, 4, T41);
  SET(d, a, b, c, 0, 11, T42);
  SET(c, d, a, b, 3, 16, T43);
  SET(b, c, d, a, 6, 23, T44);
  SET(a, b, c, d, 9, 4, T45);
  SET(d, a, b, c, 12, 11, T46);
  SET(c, d, a, b, 15, 16, T47);
  SET(b, c, d, a, 2, 23, T48);
#undef SET

/* Round 4. */
/* Let [abcd k s t] denote the operation
     a = b + ((a + I(b,c,d) + X[k] + T[i]) <<< s). */
#define I(x, y, z) ((y) ^ ((x) | ~(z)))
#define SET(a, b, c, d, k, s, Ti)                                             \
  t = a + I(b, c, d) + X[k] + (Ti);                                           \
  a = ROTATE_LEFT(t, s) + b
  /* Do the following 16 operations. */
  SET(a, b, c, d, 0, 6, T49);
  SET(d, a, b, c, 7, 10, T50);
  SET(c, d, a, b, 14, 15, T51);
  SET(b, c, d, a, 5, 21, T52);
  SET(a, b, c, d, 12, 6, T53);
  SET(d, a, b, c, 3, 10, T54);
  SET(c, d, a, b, 10, 15, T55);
  SET(b, c, d, a, 1, 21, T56);
  SET(a, b, c, d, 8, 6, T57);
  SET(d, a, b, c, 15, 10, T58);
  SET(c, d, a, b, 6, 15, T59);
  SET(b, c, d, a, 13, 21, T60);
  SET(a, b, c, d, 4, 6, T61);
  SET(d, a, b, c, 11, 10, T62);
  SET(c, d, a, b, 2, 15, T63);
  SET(b, c, d, a, 9, 21, T64);
#undef SET

  /* Then perform the following additions. (That is increment each
     of the four registers by the value it had before this block
     was started.) */
  pms->abcd[0] += a;
  pms->abcd[1] += b;
  pms->abcd[2] += c;
  pms->abcd[3] += d;
}

/* Initialize the algorithm. */
static void md5_init(md5_state_t* pms)
{
  pms->count[0] = pms->count[1] = 0;
  pms->abcd[0] = 0x67452301;
  pms->abcd[1] = /*0xefcdab89*/ T_MASK ^ 0x10325476;
  pms->abcd[2] = /*0x98badcfe*/ T_MASK ^ 0x67452301;
  pms->abcd[3] = 0x10325476;
}

/* Append a string to the message. */
static void md5_append(md5_state_t* pms, const md5_byte_t* data, size_t nbytes)
{
  const md5_byte_t* p = data;
  size_t left = nbytes;
  size_t offset = (pms->count[0] >> 3) & 63;
  md5_word_t nbits = (md5_word_t)(nbytes << 3);

  if (nbytes <= 0) {
    return;
  }

  /* Update the message length. */
  pms->count[1] += (md5_word_t)(nbytes >> 29);
  pms->count[0] += nbits;
  if (pms->count[0] < nbits) {
    pms->count[1]++;
  }

  /* Process an initial partial block. */
  if (offset) {
    size_t copy = (offset + nbytes > 64 ? 64 - offset : nbytes);

    memcpy(pms->buf + offset, p, copy);
    if (offset + copy < 64) {
      return;
    }
    p += copy;
    left -= copy;
    md5_process(pms, pms->buf);
  }

  /* Process full blocks. */
  for (; left >= 64; p += 64, left -= 64) {
    md5_process(pms, p);
  }

  /* Process a final partial block. */
  if (left) {
    memcpy(pms->buf, p, left);
  }
}

/* Finish the message and return the digest. */
static void md5_finish(md5_state_t* pms, md5_byte_t digest[16])
{
  static const md5_byte_t pad[64] = { 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  md5_byte_t data[8];
  int i;

  /* Save the length before padding. */
  for (i = 0; i < 8; ++i) {
    data[i] = (md5_byte_t)(pms->count[i >> 2] >> ((i & 3) << 3));
  }
  /* Pad to 56 bytes mod 64. */
  md5_append(pms, pad, ((55 - (pms->count[0] >> 3)) & 63) + 1);
  /* Append the length. */
  md5_append(pms, data, 8);
  for (i = 0; i < 16; ++i) {
    digest[i] = (md5_byte_t)(pms->abcd[i >> 2] >> ((i & 3) << 3));
  }
}

#if defined(__clang__) && !defined(__INTEL_COMPILER)
#  pragma clang diagnostic pop
#endif

/* Wrap up the MD5 state in our opaque structure.  */
struct kwsysMD5_s
{
  md5_state_t md5_state;
};

kwsysMD5* kwsysMD5_New(void)
{
  /* Allocate a process control structure.  */
  kwsysMD5* md5 = (kwsysMD5*)malloc(sizeof(kwsysMD5));
  if (!md5) {
    return 0;
  }
  return md5;
}

void kwsysMD5_Delete(kwsysMD5* md5)
{
  /* Make sure we have an instance.  */
  if (!md5) {
    return;
  }

  /* Free memory.  */
  free(md5);
}

void kwsysMD5_Initialize(kwsysMD5* md5)
{
  md5_init(&md5->md5_state);
}

void kwsysMD5_Append(kwsysMD5* md5, unsigned char const* data, int length)
{
  size_t dlen;
  if (length < 0) {
    dlen = strlen((char const*)data);
  } else {
    dlen = (size_t)length;
  }
  md5_append(&md5->md5_state, (md5_byte_t const*)data, dlen);
}

void kwsysMD5_Finalize(kwsysMD5* md5, unsigned char digest[16])
{
  md5_finish(&md5->md5_state, (md5_byte_t*)digest);
}

void kwsysMD5_FinalizeHex(kwsysMD5* md5, char buffer[32])
{
  unsigned char digest[16];
  kwsysMD5_Finalize(md5, digest);
  kwsysMD5_DigestToHex(digest, buffer);
}

void kwsysMD5_DigestToHex(unsigned char const digest[16], char buffer[32])
{
  /* Map from 4-bit index to hexadecimal representation.  */
  static char const hex[16] = { '0', '1', '2', '3', '4', '5', '6', '7',
                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

  /* Map each 4-bit block separately.  */
  char* out = buffer;
  int i;
  for (i = 0; i < 16; ++i) {
    *out++ = hex[digest[i] >> 4];
    *out++ = hex[digest[i] & 0xF];
  }
}
