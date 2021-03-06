/* -*- c-basic-offset: 4 -*- */
/*
 * This file is part of the freepv panoramic viewer.
 *
 *  Author: Brian Greenstone <brian@pangeasoft.net>
 *
 *  Modified for FreePV by Pablo d'Angelo <pablo.dangelo@web.de>
 *
 *  $Id: QTVRDecoder.cpp 156 2009-02-22 10:59:11Z brunopostle $
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; version 2.1 of
 * the License
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this software; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA, or see the FSF site: http://www.fsf.org.
 */
/*

	SPECIAL NOTE:
	-----------------------------------------------
	
	This Quicktime .mov parser sucks!  I can't belive it actually works.  
	It will parse through the Atoms in the file and try it's best to handle them.
	However, the shoddy documentation on this made it impossible to completely figure out,
	so hopefully this will simply work.

    dangelo: I have improved this parser quite a lot. it now supports multiple samples per chunk,
             correctly detects the full resolution track and also decodes cylindrical panoramas.
             However, the code became even more messy...
*/

#include <math.h>
#include <errno.h>
#include <vector>
#include <cstring>
#include <cstdio>

#include <zlib.h>

#include "utils.h"
#include "JpegReader.h"
#include "QTVRDecoder.h"
#include "Parameters.h"

namespace FPV
{

    typedef unsigned long u_long;
    typedef bool Boolean;

    typedef int32 DWORD;
    typedef short WORD;

    struct ChunkOffsetAtom
    {
        int32 size;
        int32 type;
        char version;
        char flags[3];
        int32 numEntries;
        int32 chunkOffsetTable[200];
    };

    struct SampleSizeAtom
    {
        int32 size;
        int32 type;
        char version;
        char flags[3];
        int32 sampleSize;
        int32 numEntries;
        int32 sampleSizeTable[200];
    };

    struct PublicHandlerInfo
    {
        int32 componentType;
        int32 componentSubType;
    };

    struct HandlerAtom
    {
        int32 size;
        int32 type;
        char version;
        char flags[3];
        PublicHandlerInfo hInfo;
    };

    struct QTVRCubicViewAtom
    {
        float minPan;
        float maxPan;
        float minTilt;
        float maxTilt;
        float minFieldOfView;
        float maxFieldOfView;
        float defaultPan;
        float defaultTilt;
        float defaultFieldOfView;
    };

    // 'pdat' atom
    struct VRPanoSampleAtom
    {
        WORD majorVersion;
        WORD minorVersion;
        DWORD imageRefTrackIndex;
        DWORD hotSpotRefTrackIndex;
        float minPan;
        float maxPan;
        float minTilt;
        float maxTilt;
        float minFieldOfView;
        float maxFieldOfView;
        float defaultPan;
        float defaultTilt;
        float defaultFieldOfView;
        DWORD imageSizeX;
        DWORD imageSizeY;
        WORD imageNumFramesX;
        WORD imageNumFramesY;
        DWORD hotSpotSizeX;
        DWORD hotSpotSizeY;
        WORD hotSpotNumFramesX;
        WORD hotSpotNumFramesY;
        DWORD flags;
        DWORD panoType;
        DWORD reserved2;
    };

// 'cube'
#define kQTVRCube 'cube'
//0x65627563

// 'hcyl'
#define kQTVRHorizontalCylinder 'hcyl'
//0x6C796368

// 'vcyl'
#define kQTVRVerticalCylinder 'vcyl'
    //0x6C796376

    struct QTVRCubicFaceData
    {
        float orientation[4];
        float center[2];
        float aspect;
        float skew;
    };

    struct QTVRTrackRefEntry
    {
        uint32 trackRefType;
        uint16 trackResolution;
        uint32 trackRefIndex;
    };

#define DECOMP_CHUNK 4096

    /* Decompress from file source to file dest until stream ends or EOF.
   inf() returns Z_OK on success, Z_MEM_ERROR if memory could not be
   allocated for processing, Z_DATA_ERROR if the deflate data is
   invalid or incomplete, Z_VERSION_ERROR if the version of zlib.h and
   the version of the library linked do not match, or Z_ERRNO if there
   is an error reading or writing the files. */
    int decompressZLIBFile(FILE *source, FILE *dest)
    {
        int ret;
        unsigned have;
        z_stream strm;
        unsigned char in[DECOMP_CHUNK];
        unsigned char out[DECOMP_CHUNK];

        /* allocate inflate state */
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.avail_in = 0;
        strm.next_in = Z_NULL;
        ret = inflateInit(&strm);
        if (ret != Z_OK)
            return ret;

        /* decompress until deflate stream ends or end of file */
        do
        {
            strm.avail_in = fread(in, 1, DECOMP_CHUNK, source);
            if (ferror(source))
            {
                (void)inflateEnd(&strm);
                return Z_ERRNO;
            }
            if (strm.avail_in == 0)
                break;
            strm.next_in = in;

            /* run inflate() on input until output buffer not full */
            do
            {
                strm.avail_out = DECOMP_CHUNK;
                strm.next_out = out;
                ret = inflate(&strm, Z_NO_FLUSH);
                assert(ret != Z_STREAM_ERROR); /* state not clobbered */
                switch (ret)
                {
                case Z_NEED_DICT:
                    ret = Z_DATA_ERROR; /* and fall through */
                case Z_DATA_ERROR:
                case Z_MEM_ERROR:
                    (void)inflateEnd(&strm);
                    return ret;
                }
                have = DECOMP_CHUNK - strm.avail_out;
                if (fwrite(out, 1, have, dest) != have || ferror(dest))
                {
                    (void)inflateEnd(&strm);
                    return Z_ERRNO;
                }
            } while (strm.avail_out == 0);

            /* done when inflate() says it's done */
        } while (ret != Z_STREAM_END);

        /* clean up and return */
        (void)inflateEnd(&strm);
        return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
    }

    /**************** PARSE QUICKTIME MOVIE ***********************/
    //
    // Parses through the QT movie's atoms looking for our 6 cube textures.
    //

    QTVRDecoder::QTVRDecoder()
    {
        gCurrentTrackMedia = 0;
        //	gAlreadyGotVideoMedia = false;
        gFoundJPEGs = false;
        m_imageRefTrackIndex = 0;
        m_panoType = 0;
        m_mainTrack = 0;
        m_currTrackIsImageTrack = false;
        m_type = Parameters::PANO_UNKNOWN;
        m_cmovZLib = false;

        // determine byteorder
        int testint = 0x01;
        unsigned char *testchar = reinterpret_cast<unsigned char *>(&testint);
        if (testchar[0] == 0x01)
            m_HostBigEndian = false;
        else
            m_HostBigEndian = true;
    }

    QTVRDecoder::~QTVRDecoder()
    {
        /**************/
        /* CLOSE FILE */
        /**************/

        fclose(gFile);
    }

    bool QTVRDecoder::parseHeaders(const char *theDataFilePath)
    {

        bool ok = true;
        int32 atomSize;
        gFile = fopen(theDataFilePath, "rb");
        if (!gFile)
        {
            DEBUG_ERROR("fopen() failed: " << strerror(errno));
            return false;
        }
        m_mainFile = gFile;

        // get file size for EOF test
        size_t filepos = ftell(gFile);
        fseek(gFile, 0, SEEK_END);
        size_t filesize = ftell(gFile);
        fseek(gFile, filepos, SEEK_SET);

        /*************************/
        /* RECURSE THROUGH ATOMS */
        /*************************/
        do
        {
            atomSize = ReadMovieAtom();
        } while (atomSize > 0 && ftell(gFile) < filesize);

        if (m_error != "")
        {
            return false;
        }

        //gMyInstances[instanceNum].drawDecodingBar = false;

        return (ok);
    }

    /************************************************/
    /* Read a QT atom                               */

    long QTVRDecoder::ReadQTMovieAtom(void)
    {
        int32 atomSize, atomType, remainingSize;
        int16 childCount;
        size_t filePos;
        //char    *c = (char *)&atomType;
        //OSErr iErr;

        /* GET CURRENT FILE POS */

        filePos = ftell(gFile);

        /* READ THE ATOM SIZE */

        size_t sz = fread(&atomSize, 1, 4, gFile);
        if (ferror(gFile) || sz != 4)
        {
            printf("ReadMovieAtom:  fread() failed!\n");
            return (-1);
        }

        /* READ THE ATOM TYPE */

        sz = fread(&atomType, 1, 4, gFile);
        if (ferror(gFile) || sz != 4)
        {
            printf("ReadMovieAtom:  fread() failed!\n");
            return (-1);
        }

        // skip id and reserved
        fseek(gFile, 6, SEEK_CUR);

        sz = fread(&childCount, 1, 2, gFile);
        if (ferror(gFile) || sz != 2)
        {
            printf("ReadMovieAtom:  fread() failed!\n");
            return (-1);
        }

        // skip reserved
        fseek(gFile, 4, SEEK_CUR);

        Swizzle(&atomSize); // convert BigEndian data to LittleEndian
        Swizzle(&atomType);
        Swizzle(&childCount);

        //printf("QTAtom 0x%08X  (0x%08X)  %c%c%c%c child count: %d\n", filePos, (int)atomSize, *(c +3), *(c + 2), *(c + 1) ,*c, childCount );

        /* READ EXTENDED DATA IF NEEDED */

        if (atomSize == 1) // if atom size == 1 then there's extended data in the header
        {
            printf("ReadMovieAtom: Extended size isn't supported yet...\n");
            return (-1);
        }

        /********************/
        /* HANDLE THIS ATOM */
        /********************/
        //
        // Any atom types not in the table below just get skipped.
        //

        switch (atomType)
        {
        case 'sean':
            // recurse into sub atoms
            remainingSize = atomSize - 20; // there are n bytes left in this atom to parse
                                           /*            do
            {
                remainingSize -= ReadMovieAtom();           // read atom and dec by its size                    
            }
            while(remainingSize > 0);
*/
            //printf("  [Subrecursing 'sean' qt atom]\n");
            for (int i = 0; i < childCount; i++)
            {
                remainingSize -= ReadQTMovieAtom();
            }
            //printf("  [End subrecursing 'sean' qt atom]\n");
            break;
        case 'tref':
            ReadAtom_QTVR_TREF(atomSize - 20);
            break;
        case 'pdat':
            ReadAtom_QTVR_PDAT(atomSize - 20);
            break;
        }

        //if (iErr != noErr)
        //  return(-1);

        /*************************/
        /* SET FPOS TO NEXT ATOM */
        /*************************/

        if (atomSize == 0) // if last atom size was 0 then that was the end
        {
            printf("\n===== that should have been the end.\n");
            return (-1);
        }
        else
        {
            int r = fseek(gFile, (long)filePos + atomSize, SEEK_SET);
            if (ferror(gFile) || r != 0)
                printf("ReadQTMovieAtom: fseek() failed, probably EOF?\n");
        }

        return (atomSize); // return the size of the atom
    }

    /********************* READ MOVIE ATOM ************************/
    //
    // INPUT:  parentAtomSize = size of the enclosing atom (or -1 if root).
    //						This is used to determine when we've read all the child atoms from
    //						the enclosing parent that we sub-recursed.
    //
    // OUTPUT:	size of the atom
    //

    long QTVRDecoder::ReadMovieAtom(void)
    {
        int32 atomSize, atomType, remainingSize;
        size_t filePos;
        //char	*c = (char *)&atomType;
        //OSErr	iErr;

        /*******/
        /* MAC */
        /*******/

#ifdef TARGET_OS_MAC
        /* GET CURRENT FILE POS */
        //
        // We get the current file position, so that we can add the atom size
        // to skip over it later.
        //

        GetFPos(gMovfRefNum, &filePos);

        /* READ THE ATOM SIZE */

        count = 4; // size is 4 bytes
        iErr = FSRead(gMovfRefNum, &count, &atomSize);
        if (iErr)
        {
            printf("ReadMovieAtom:  FSRead failed!  %d\n", iErr);
            return (-1);
        }

        /* READ THE ATOM TYPE */

        count = 4; // type is 4 bytes
        if (FSRead(gMovfRefNum, &count, &atomType) != noErr)
        {
            printf("ReadMovieAtom:  FSRead failed!\n");
            return (1);
        }

        /***********/
        /* WINDOWS */
        /***********/

#else
        /* GET CURRENT FILE POS */

        filePos = ftell(gFile);

        /* READ THE ATOM SIZE */

        size_t sz = fread(&atomSize, 1, 4, gFile);
        if (ferror(gFile) || sz != 4)
        {
            printf("ReadMovieAtom:  fread() failed!\n");
            return (-1);
        }

        /* READ THE ATOM TYPE */

        sz = fread(&atomType, 1, 4, gFile);
        if (ferror(gFile) || sz != 4)
        {
            printf("ReadMovieAtom:  fread() failed!\n");
            return (-1);
        }

        Swizzle(&atomSize); // convert BigEndian data to LittleEndian
        Swizzle(&atomType);

#endif

        //printf("Atom 0x%08X  (0x%08X)  %c%c%c%c \n", filePos, (int)atomSize, *(c +3), *(c + 2), *(c + 1) ,*c  );

        /* READ EXTENDED DATA IF NEEDED */

        if (atomSize == 1) // if atom size == 1 then there's extended data in the header
        {
            printf("ReadMovieAtom: Extended size isn't supported yet...\n");
            return (-1);
        }

        /********************/
        /* HANDLE THIS ATOM */
        /********************/
        //
        // Any atom types not in the table below just get skipped.
        //

        switch (atomType)
        {
            /* MOOV */
            //
            // This contains more sub-atoms, so just recurse
            //

        case 'moov': // MovieAID:										//'moov'
            //printf("  [Subrecursing 'moov' atom]\n");
            remainingSize = atomSize - 8; // there are n bytes left in this atom to parse
            do
            {
                remainingSize -= ReadMovieAtom(); // read atom and dec by its size
            } while (remainingSize > 0);
            //printf("  [End subrecurse 'moov' atom]\n");
            break;

            /* CMOV */
            //
            // This contains more sub-atoms, so just recurse
            //

        case 'cmov': // MovieAID:                                        //'moov'
            //printf("  [Subrecursing 'cmov' atom]\n");
            remainingSize = atomSize - 8; // there are n bytes left in this atom to parse
            do
            {
                remainingSize -= ReadMovieAtom(); // read atom and dec by its size
            } while (remainingSize > 0);
            //printf("  [End subrecurse 'cmov' atom]\n");
            break;

        case 'dcom':
            ReadAtom_DCOM(atomSize - 8);
            break;

        case 'cmvd':
            ReadAtom_CMVD(atomSize - 8);
            break;

            /* TRAK */

        case 'trak': //TrackAID:										//'trak'
                     //printf("  [Subrecursing 'trak' atom]\n");
            // reset per track information
            m_currTrackIsImageTrack = false;
            gCurrentTrackMedia = 0;
            remainingSize = atomSize - 8; // there are n bytes left in this atom to parse
            do
            {
                remainingSize -= ReadMovieAtom(); // read atom and dec by its size
            } while (remainingSize > 0);
            //printf("  [End subrecurse 'trak' atom]\n");
            break;

        case 'tkhd':
            ReadAtom_TKHD(atomSize - 8);
            break;

            /* TREF */
        case 'tref':
            ReadAtom_TREF(atomSize - 8);
            break;

            /* MDIA */

        case 'mdia': //MediaAID:										//'mdia'
            //printf("  [Subrecursing 'mdia' atom]\n");
            remainingSize = atomSize - 8; // there are n bytes left in this atom to parse
            do
            {
                remainingSize -= ReadMovieAtom(); // read atom and dec by its size
            } while (remainingSize > 0);
            //printf("  [End subrecurse 'mdia' atom]\n");
            break;

            /* MINF */

        case 'minf': //MediaInfoAID:									//'minf'
            //printf("  [Subrecursing 'minf' atom]\n");
            remainingSize = atomSize - 8; // there are n bytes left in this atom to parse
            do
            {
                remainingSize -= ReadMovieAtom(); // read atom and dec by its size
            } while (remainingSize > 0);
            //printf("  [End subrecurse 'minf' atom]\n");
            break;

            /* DINF */

        case 'dinf': //DataInfoAID:									//'dinf'
            //printf("  [Subrecursing 'dinf' atom]\n");
            ReadMovieAtom();
            //printf("  [End subrecurse 'dinf' atom]\n");
            break;

            /* STBL */

        case 'stbl': //SampleTableAID:									//'stbl'

            //printf("  [Subrecursing 'stbl' atom]\n");
            remainingSize = atomSize - 8; // there are n bytes left in this atom to parse
            do
            {
                remainingSize -= ReadMovieAtom(); // read atom and dec by its size
            } while (remainingSize > 0);
            //printf("  [End subrecurse 'stbl' atom]\n");
            break;

            /* STCO */

        case 'stco': // STChunkOffsetAID:								//'stco'
            ReadAtom_STCO(atomSize);
            break;

            /* STSZ */

        case 'stsz': //STSampleSizeAID:								//'stsz'
            ReadAtom_STSZ(atomSize);
            break;

        case 'stsc':
            ReadAtom_STSC(atomSize);
            break;

            /* HDLR */

        case 'hdlr': //HandlerAID:								//'hdlr'
            ReadAtom_HDLR(atomSize);
            break;
        }

        //if (iErr != noErr)
        //	return(-1);

        /*************************/
        /* SET FPOS TO NEXT ATOM */
        /*************************/

        if (atomSize == 0) // if last atom size was 0 then that was the end
        {
            printf("\n===== that should have been the end.\n");
            return (-1);
        }
        else
        {
#ifdef TARGET_OS_MAC
            if (SetFPos(gMovfRefNum, fsFromStart, filePos + atomSize))
                printf("ReadMovieAtom: SetFPos failed, probably EOF?\n");
#else
            fseek(gFile, (long)filePos + atomSize, SEEK_SET);
            if (ferror(gFile))
                printf("ReadMovieAtom: fseek() failed, probably EOF?\n");
#endif
        }

        return (atomSize); // return the size of the atom
    }

    /********************** READ ATOM:  DCOM ****************************/

    void QTVRDecoder::ReadAtom_DCOM(long size)
    {
        char comp[5];
        comp[4] = 0;
        //int32         count;
        //    HandlerAtom     *atom;
        //    PublicHandlerInfo   *info;
        //    int32           componentSubType;

        size_t sz = fread(&comp, 1, 4, gFile);
        if (ferror(gFile) || sz != 4)
        {
            printf("ReadAtom_DCOM:  fread() failed!\n");
            return;
        }

        DEBUG_DEBUG("compression type: " << comp);

        if (strcmp(comp, "zlib") != 0)
        {
            m_error = std::string("unsupported compressed header: ") + comp;
            return;
        }
        m_cmovZLib = true;
    }

    /********************** READ ATOM:  CMVD****************************/

    void QTVRDecoder::ReadAtom_CMVD(long size)
    {
        int32 uncomp_size;

        size_t sz = fread(&uncomp_size, 1, 4, gFile);
        if (ferror(gFile) || sz != 4)
        {
            printf("ReadAtom_CMVD:  fread() failed!\n");
            return;
        }
        size -= (int)sz;

        if (m_cmovZLib)
        {
            // decompress compressed header
            // create temporary file
            FILE *decomp = tmpfile();
            if (!decomp)
            {
                // error
                m_error = "Could not open temporary file for header decompression";
                return;
            }

            if (decompressZLIBFile(gFile, decomp) != Z_OK)
            {
                m_error = "zlib decompression failed";
                fclose(decomp);
                return;
            }
            // restart parser on now decompressed header.
            fseek(decomp, 0, SEEK_SET);
            m_mainFile = gFile;
            m_cmovFile = decomp;

            gFile = m_cmovFile;
            /*************************/
            /* RECURSE THROUGH ATOMS */
            /*************************/
            int atomSize;
            do
            {
                atomSize = ReadMovieAtom();
            } while (atomSize > 0);

            // switch back to main file
            gFile = m_mainFile;

            // close header file
            fclose(m_cmovFile);
        }
    }

    /********************** READ ATOM:  STCO ****************************/

    void QTVRDecoder::ReadAtom_STCO(long size)
    {
        //int32			count;
        int i;
        ChunkOffsetAtom *atom;
        int numEntries;

        /*****************/
        /* READ THE ATOM */
        /*****************/

#ifdef TARGET_OS_MAC
        SetFPos(gMovfRefNum, fsFromMark, -8); // back up 8 bytes to the atom's start so we can read it all in
#else
        fseek(gFile, -8, SEEK_CUR);
#endif

        /* ALLOC MEMORY FOR IT */
        //
        // This is a variable size structure, so we need to allocated based on the size of the atom that's passed in
        //

        atom = (FPV::ChunkOffsetAtom *)malloc(size);

        /* READ IT */

#ifdef TARGET_OS_MAC
        count = size;
        if (FSRead(gMovfRefNum, &count, atom) != noErr)
        {
            printf("ReadAtom_STCO:  FSRead failed!\n");
            return;
        }
#else
        fread(atom, size, 1, gFile);
        if (ferror(gFile))
        {
            printf("ReadAtom_STCO:  fread() failed!\n");
            return;
        }
#endif

        /* SEE WHAT KIND OF TRACK WE'VE PARSED INTO AND GET CHUNKS BASED ON THAT */

        numEntries = atom->numEntries;
        Swizzle(&numEntries); // convert BigEndian data to LittleEndian (if not Mac)

        switch (gCurrentTrackMedia)
        {
        case 'pano':
        {
            gPanoChunkOffset = atom->chunkOffsetTable[0];
            Swizzle(&gPanoChunkOffset); // convert BigEndian data to LittleEndian (if not Mac)
                                        //printf("        Chunk offset to 'pano' is : %d\n", gPanoChunkOffset);
            // TODO: parse the pano info atom here!
            long fpos_saved = ftell(gFile);

            bool switchChunk = (m_cmovFile == gFile);
            // the pano chunk is always stored in the main file..
            if (switchChunk)
            {
                gFile = m_mainFile;
            }

            // seek to panorama description. skip first 12 bytes
            // of new QT atom.
            fseek(gFile, gPanoChunkOffset + 12, SEEK_SET);

            //printf("  [Subrecursing pano 'stco' atom]\n");
            size_t remainingSize = gPanoSampleSize - 12; // there are n bytes left in this atom to parse
            do
            {
                remainingSize -= ReadQTMovieAtom(); // read atom and dec by its size
            } while (remainingSize > 0);
            //printf("  [End subrecurse pano 'stco' atom]\n");

            // switch back to compressed file, if needed
            if (switchChunk)
            {
                gFile = m_cmovFile;
            }

            fseek(gFile, fpos_saved, SEEK_SET);
            gCurrentTrackMedia = 0; // reset this now!
        }
        break;

        case 'vide':

            // if this is the main track, extract the images
            if (m_currTrackIsImageTrack)
            {
                /* EXTRACT THE OFFSETS TO THE IMAGES */
#if 0							
				for (i = 0; i < numEntries; i++)
				{
					printf("       # Chunk Offset entries: %d\n", numEntries);

					for (i = 0; i < numEntries; i++)
					{
						gVideoChunkOffset[i] = atom->chunkOffsetTable[i];
						Swizzle(&gVideoChunkOffset[i]);							// convert BigEndian data to LittleEndian (if not Mac)
						printf("       Chunk offset #%d = %d\n", i, gVideoChunkOffset[i] );
					}
					gCurrentTrackMedia = 0;											// reset this now!
					break;
				}
#endif
                // consider the sample to chunk atom when writing the offsets
                m_sample2ChunkTable[0];
                int sampleTableId = 0;
                int chunkId = 0;
                int32 sampleOffset = atom->chunkOffsetTable[0];
                Swizzle(&sampleOffset);
                // count sample in current chunk
                int samplesInChunk = 0;
                for (i = 0; i < gNumTilesPerImage * 6; i++)
                {
                    // check if we have reached the end of the current Chunk
                    if (m_sample2ChunkTable[sampleTableId].samplesPerChunk == samplesInChunk)
                    {
                        // we have reached a new chunk
                        chunkId++;
                        samplesInChunk = 0;
                        if (sampleTableId < ((int)m_sample2ChunkTable.size()) - 1)
                        {
                            // check if we have reached a sample to chunk table entry
                            if (chunkId + 1 == m_sample2ChunkTable[sampleTableId + 1].startChunk)
                            {
                                // advance to next entry in sample table
                                sampleTableId++;
                            }
                        }
                        else
                        {
                            // we are in the last entry of the sample to chunktable, no need to check for a new entry
                        }
                        // update chunk offset
                        sampleOffset = atom->chunkOffsetTable[chunkId];
                        Swizzle(&sampleOffset);
                    }
                    gVideoChunkOffset[i] = sampleOffset;
                    // advance to next sample
                    sampleOffset += gVideoSampleSize[i];
                    samplesInChunk++;
                }
                gCurrentTrackMedia = 0;
            }
        }

        //bail:
        free(atom);
    }

    /********************** READ ATOM:  STSZ ****************************/

    void QTVRDecoder::ReadAtom_STSZ(long size)
    {
        //int32			count;
        SampleSizeAtom *atom;
        int32 numEntries, i;

        /*****************/
        /* READ THE ATOM */
        /*****************/

#ifdef TARGET_OS_MAC
        SetFPos(gMovfRefNum, fsFromMark, -8); // back up 8 bytes to the atom's start so we can read it all in
#else
        fseek(gFile, -8, SEEK_CUR);
#endif

        /* ALLOC MEMORY FOR IT */
        //
        // This is a variable size structure, so we need to allocated based on the size of the atom that's passed in
        //

        atom = (SampleSizeAtom *)malloc(size);

        /* READ IT */

#ifdef TARGET_OS_MAC
        count = size;
        if (FSRead(gMovfRefNum, &count, atom) != noErr)
        {
            printf("ReadAtom_STSZ:  FSRead failed!\n");
            return;
        }
#else
        fread(atom, size, 1, gFile);
        if (ferror(gFile))
        {
            printf("ReadAtom_STSZ:  fread() failed!\n");
            return;
        }
#endif

        /* SEE WHAT KIND OF TRACK WE'VE PARSED INTO AND GET CHUNKS BASED ON THAT */

        numEntries = atom->numEntries;
        Swizzle(&numEntries); // convert BigEndian data to LittleEndian (if not Mac)

        switch (gCurrentTrackMedia)
        {
        case 'pano':
            gPanoSampleSize = atom->sampleSize;
            Swizzle(&gPanoSampleSize); // convert BigEndian data to LittleEndian (if not Mac)
            //printf("        'pano' sample size = : %d\n", gPanoSampleSize);
            break;

        case 'vide':
            //printf("       # Sample Size entries: %d\n", numEntries);

            if (m_currTrackIsImageTrack)
            {

                if (m_type == Parameters::PANO_CUBIC)
                {
                    if (numEntries < 6) // there MUST be at least 6 jpegs or this isn't a cube
                    {
                        printf("THERE ARE NOT 6 JPEGS IN THIS FILE!  We only support cubic QTVR's, and those have 6 or more JPEGs!\n");
                        printf("This appears to only have %d\n", numEntries);
                        m_error = "cubic panorama with less than 6 image";
                        free(atom);
                        return;
                    }

                    gFoundJPEGs = true;

                    /* ARE THE IMAGES TILED? */

                    gNumTilesPerImage = numEntries / 6;

                    if (gNumTilesPerImage > 1)
                    {
                        //printf("_____ There are more than 6 entires in the 'vide' track, so this QTVR has tiled images!\n");
                        gImagesAreTiled = true;
                        if (numEntries > MAX_IMAGE_OFFSETS) // are there too many tiles
                        {
                            printf("THERE APPEAR TO BE TOO MANY TILE IMAGES IN THIS FILE!!!!!!!  %d\n", numEntries);
                            free(atom);
                            return;
                        }
                    }
                    else
                    {
                        gImagesAreTiled = false;
                    }
                }
                else
                {
                    // cylindrical pano
                    gFoundJPEGs = true;
                    gNumTilesPerImage = numEntries;
                    if (gNumTilesPerImage > 1)
                    {
                        //printf("_____ There are more than 1 entires in the 'vide' track, so this QTVR has a tiled image!\n");
                        gImagesAreTiled = true;
                        if (numEntries > MAX_IMAGE_OFFSETS) // are there too many tiles
                        {
                            printf("THERE APPEAR TO BE TOO MANY TILE IMAGES IN THIS FILE!!!!!!!  %d\n", numEntries);
                            free(atom);
                            return;
                        }
                    }
                    else
                    {
                        gImagesAreTiled = false;
                    }
                }
                for (i = 0; i < numEntries; i++)
                {
                    gVideoSampleSize[i] = atom->sampleSizeTable[i];
                    Swizzle(&gVideoSampleSize[i]);
                    //printf("       sample size %d = %d\n", i, gVideoSampleSize[i] );
                }
                break;
            }
        }

        free(atom);
    }

    /********************** READ ATOM:  STSC ****************************/

    void QTVRDecoder::ReadAtom_STSC(long size)
    {
        int32 numEntries;

        // skip version and flags
        size_t sz = fread(&numEntries, 1, 4, gFile);
        if (ferror(gFile) || sz != 4)
        {
            printf("ReadAtom_STSC:  fread() failed!\n");
            return;
        }

        // read number of entries
        sz = fread(&numEntries, 1, 4, gFile);
        if (ferror(gFile) || sz != 4)
        {
            printf("ReadAtom_STSC:  fread() failed!\n");
            return;
        }
        Swizzle(&numEntries);

        // discart old sample table
        m_sample2ChunkTable.clear();
        for (int i = 0; i < numEntries; i++)
        {
            SampleToChunkEntry tmp;
            sz = fread(&tmp, 1, 12, gFile);
            if (ferror(gFile) || sz != 12)
            {
                printf("ReadAtom_STSC:  fread() failed!\n");
                return;
            }
            Swizzle(&tmp.startChunk);
            Swizzle(&tmp.samplesPerChunk);
            Swizzle(&tmp.sampleDescriptionID);
            DEBUG_DEBUG("Adding sample2chunk: chunk " << tmp.startChunk << " # samp " << tmp.samplesPerChunk << " descrID " << tmp.sampleDescriptionID);
            m_sample2ChunkTable.push_back(tmp);
        }
    }

    /********************** READ ATOM:  HDLR ****************************/

    void QTVRDecoder::ReadAtom_HDLR(int size)
    {
        //int32			count;
        HandlerAtom *atom;
        PublicHandlerInfo *info;
        int32 componentSubType;

        /*****************/
        /* READ THE ATOM */
        /*****************/

#ifdef TARGET_OS_MAC
        SetFPos(gMovfRefNum, fsFromMark, -8); // back up 8 bytes to the atom's start so we can read it all in
#else
        fseek(gFile, -8, SEEK_CUR);
#endif

        /* ALLOC MEMORY FOR IT */
        //
        // This is a variable size structure, so we need to allocated based on the size of the atom that's passed in
        //

        atom = (HandlerAtom *)malloc(size);

        /* READ IT */

#ifdef TARGET_OS_MAC
        count = size;
        if (FSRead(gMovfRefNum, &count, atom) != noErr)
        {
            printf("ReadAtom_HDLR:  FSRead failed!\n");
            return;
        }
#else
        fread(atom, size, 1, gFile);
        if (ferror(gFile))
        {
            printf("ReadAtom_HDLR:  fread() failed!\n");
            return;
        }
#endif

        /* POINT TO HANDLER INFO */

        info = &atom->hInfo;

        componentSubType = info->componentSubType; // get comp sub type
        Swizzle(&componentSubType);                // convert BigEndian data to LittleEndian (if not Mac)
        char *t = (char *)&componentSubType;
        DEBUG_DEBUG("componentSubType: " << t[0] << t[1] << t[2] << t[3]);

        if (componentSubType == 'pano')
        {
            //printf("ReadAtom_HDLR:  We found the 'pano' media!\n");
            gCurrentTrackMedia = 'pano';
        }
        else if (componentSubType == 'vide')
        {
            gCurrentTrackMedia = 'vide';
            //printf("ReadAtom_HDLR:  We found a 'vide' media!\n");
#if 0
//		if (!gAlreadyGotVideoMedia)					// if we already got the 'vide' then this one is just the fast-start track, so ignore it!
		{
			gCurrentTrackMedia = 'vide';
//			gAlreadyGotVideoMedia = true;
		}
		else
			//printf("Found an additional 'vide' media track, but we're going to ignore it since it's probably the fast-start track...\n");
#endif
        }

        free(atom);
    }

    void QTVRDecoder::ReadAtom_TKHD(long size)
    {
        int32 trackid;
        //int32         count;
        //    HandlerAtom     *atom;
        //    PublicHandlerInfo   *info;
        //    int32           componentSubType;

        int ret = fseek(gFile, 12, SEEK_CUR);
        if (ret != 0)
        {
            printf("ReadAtom_TKHD:  fseek() failed!\n");
            return;
        }

        size_t sz = fread(&trackid, 1, 4, gFile);
        if (ferror(gFile) || sz != 4)
        {
            printf("ReadAtom_TKHD:  fread() failed!\n");
            return;
        }
        Swizzle(&trackid);

        DEBUG_DEBUG("track id: " << trackid);

        if (trackid == m_mainTrack)
        {
            DEBUG_DEBUG("This is the full resolution image track: " << trackid);
            // this is the main pano track.
            m_currTrackIsImageTrack = true;
        }
    }

    void QTVRDecoder::ReadAtom_TREF(long size)
    {
        int32 subsize;
        int32 type;
        int32 track;
        //int32         count;
        //    HandlerAtom     *atom;
        //    PublicHandlerInfo   *info;
        //    int32           componentSubType;

        // loop until everything has been read
        while (size)
        {
            size_t sz = fread(&subsize, 1, 4, gFile);
            if (ferror(gFile) || sz != 4)
            {
                printf("ReadAtom_TREF:  fread() failed!\n");
                return;
            }
            Swizzle(&subsize);
            subsize -= sz;
            size -= sz;
            sz = fread(&type, 1, 4, gFile);
            if (ferror(gFile) || sz != 4)
            {
                printf("ReadAtom_TREF:  fread() failed!\n");
                return;
            }
            Swizzle(&type);
            subsize -= sz;
            size -= sz;
            //int nRefs = (size)/4;
            // only store tracks of type imgt, since these provide the image tracks.
            int i = 0;
            if (type == 'imgt')
            {
                while (subsize)
                {
                    sz = fread(&track, 1, 4, gFile);
                    if (ferror(gFile) || sz != 4)
                    {
                        printf("ReadAtom_TREF:  fread() failed!\n");
                        return;
                    }
                    subsize -= sz;
                    size -= sz;
                    Swizzle(&track);
                    DEBUG_DEBUG("adding imgt track: " << track);
                    if (i < MAX_REF_TRACKS)
                        m_panoTracks[i] = track;
                    else
                        DEBUG_ERROR("maximum number of reference tracks exceeded");
                    i++;
                }
            }
            else
            {
                // seek to next atom
                int ret = fseek(gFile, subsize, SEEK_CUR);
                if (ret != 0)
                {
                    printf("ReadAtom_TREF:  fseek() failed!\n");
                    return;
                }
                size -= sz;
                subsize -= sz;
            }
        }
    }

    /********************** READ ATOM:  PDAT ****************************/

    void QTVRDecoder::ReadAtom_QTVR_PDAT(long size)
    {
        //int32			count;
        VRPanoSampleAtom *atom;
        //int32			numEntries, i;

        /*****************/
        /* READ THE ATOM */
        /*****************/

        /* ALLOC MEMORY FOR IT */
        //
        // This is a variable size structure, so we need to allocated based on the size of the atom that's passed in
        //

        atom = (VRPanoSampleAtom *)malloc(size);

        /* READ IT */

#ifdef TARGET_OS_MAC
        count = size;
        if (FSRead(gMovfRefNum, &count, atom) != noErr)
        {
            printf("ReadAtom_PDAT:  FSRead failed!\n");
            return;
        }
#else
        size_t sz = fread(atom, size, 1, gFile);
        if (ferror(gFile) || sz != 1)
        {
            printf("ReadAtom_PDAT:  fread() failed!\n");
            return;
        }
#endif

        /* SEE WHAT KIND OF TRACK WE'VE PARSED INTO AND GET CHUNKS BASED ON THAT */

        // check if this is a cubic panorama
        m_panoType = atom->panoType;
        Swizzle(&m_panoType); // convert BigEndian data to LittleEndian (if not Mac)
        char *t = (char *)&m_panoType;
        DEBUG_DEBUG("panoType: " << t[3] << t[2] << t[1] << t[0]);

        if (m_panoType == kQTVRCube)
        {
            // abort reading
            //        m_error = "Cylindrical panoramas are not supported yet.";
            m_type = Parameters::PANO_CUBIC;
        }
        else if (m_panoType == 'hcyl')
        {
            DEBUG_DEBUG("horizontal cylindrical panorama");
            m_type = Parameters::PANO_CYLINDRICAL;
            // orientation of panorama.
            m_horizontalCyl = true;
        }
        else if (m_panoType == 'vcyl')
        {
            DEBUG_DEBUG("vertical cylindrical panorama");
            m_type = Parameters::PANO_CYLINDRICAL;
            // orientation of panorama.
            m_horizontalCyl = false;
        }
        else if (m_panoType == 0)
        {
            // old QT format, orientation stored in flags
            m_type = Parameters::PANO_CYLINDRICAL;
            m_horizontalCyl = (atom->flags & 1);
        }

        // get the track number of the real pano
        m_imageRefTrackIndex = atom->imageRefTrackIndex;
        Swizzle(&m_imageRefTrackIndex); // convert BigEndian data to LittleEndian (if not Mac)
        DEBUG_DEBUG("imageRefTrackIndex: " << m_imageRefTrackIndex);
        m_mainTrack = m_panoTracks[m_imageRefTrackIndex - 1];
        DEBUG_DEBUG("main pano track: " << m_mainTrack);

        free(atom);
    }

    /********************** READ ATOM:  PDAT ****************************/

    void QTVRDecoder::ReadAtom_QTVR_TREF(long size)
    {
        QTVRTrackRefEntry atom;

        int n = size / 10;

        for (int i = 0; i < n; i++)
        {
            fread(&(atom.trackRefType), 1, 4, gFile);
            fread(&atom.trackResolution, 1, 2, gFile);
            fread(&atom.trackRefIndex, 1, 4, gFile);
            Swizzle(&(atom.trackRefType));
            Swizzle(&(atom.trackResolution));
            Swizzle(&(atom.trackRefIndex));
            //printf("track %d: refType: %d  Resolution: %d  Index: %d\n", i, atom.trackRefType, atom.trackResolution, atom.trackRefIndex);
        }
    }

    /********************* SEEK AND EXTRACT IMAGES ***************************/
    //
    // Finds the 6 cube faces JPEG's in the .mov file and then draws them into texture buffers.
    //

    bool QTVRDecoder::extractCubeImages(Image *cubefaces[6])
    {
        //long	count;
        int i;

        if (m_type != Parameters::PANO_CUBIC)
        {
            m_error = "not a cubic panorama";
            return false;
        }

        /* SEE IF WE NEED TO SPECIAL-CASE TILED IMAGES */

        if (gImagesAreTiled)
        {
            return SeekAndExtractImages_Tiled(cubefaces);
        }

        //printf("\n\n_______SEEK & EXTRACT IMAGES_______\n\n");

        if (!gFoundJPEGs)
        {
            printf("No usable JPEG images were found, or we didn't find 6 which is needed to make a cubic pano\n");
            return false;
        }

        for (i = 0; i < 6; i++)
        {
            DEBUG_DEBUG("extracting tile # " << i << "  chunk: " << gVideoChunkOffset[i]);
            //printf("Processing image # %d...\n", i);

            /***************************/
            /* SEEK TO THE JPEG'S DATA */
            /***************************/

            fseek(gFile, gVideoChunkOffset[i], SEEK_SET);

            cubefaces[i] = new Image;
            // decode jpeg cube face
            if (!decodeJPEG(gFile, *cubefaces[i]))
            {
                m_error = "JPEG decoding failed";
                DEBUG_ERROR(m_error);
                for (int i = 0; i < 6; i++)
                {
                    if (cubefaces[i])
                    {
                        delete cubefaces[i];
                        cubefaces[i] = 0;
                    }
                }
                return false;
            }
        }
        return true;
    }

    /********************* SEEK AND EXTRACT IMAGES:  TILED ***************************/

    bool QTVRDecoder::SeekAndExtractImages_Tiled(Image *cubefaces[6])
    {
        int i;
        int tileDimensions; //, compSize;
        int faceSize;

        //printf("\n\n_______SEEK & EXTRACT TILED IMAGES_______\n\n");

        tileDimensions = (int)sqrt((float)gNumTilesPerImage);

        //printf("tileDimensions = %d\n", tileDimensions);

        for (i = 0; i < 6; i++) // for each Cube face...
        {
            int chunkNum = i * gNumTilesPerImage;

            /* LOAD ALL OF THE TILE IMAGES FOR THIS FACE */
            //
            // We load each tile image into a temp buffer.
            //

            //printf("\nLoading tiles for Face #%d\n", i);

            // init tile assembly
            if (cubefaces[i])
            {
                delete cubefaces[i];
            }
            cubefaces[i] = 0;
            int tileSize = 0;

            // load and assemble tiles.
            for (int t = 0; t < gNumTilesPerImage; t++)
            {
                int cChunk = chunkNum + t;

                //printf("Processing tile #%d...\n", t);
                DEBUG_DEBUG(" tile: " << t << "  chunk: " << cChunk << "  offset: " << gVideoChunkOffset[cChunk]);
                /* SEEK TO THE JPEG'S DATA */

                fseek(gFile, gVideoChunkOffset[cChunk], SEEK_SET);
                if (ferror(gFile))
                {
                    printf("LoadTilesForFace:  fseek failed!\n");
                    continue;
                }

                /* DECODE IT */

                Image img;
                // decode jpg
                if (!decodeJPEG(gFile, img))
                {
                    m_error = "JPEG decoding failed";
                    DEBUG_ERROR(m_error);
                    for (int i = 0; i < 6; i++)
                    {
                        if (cubefaces[i])
                        {
                            delete cubefaces[i];
                            cubefaces[i] = 0;
                        }
                    }
                    return false;
                }

                /*
#ifdef DEBUG
        {
            std::string tfn;
            FPV_S2S(tfn, "fpv_dbg_face_" << i <<  "_tile_" << t  << ".ppm");
            img.writePPM(tfn);
        }
#endif
            */
                /* CALCULATE THE DIMENSIONS OF THE TARGET IMAGE */
                if (cubefaces[i] == 0)
                {
                    // resize cube face
                    tileSize = img.size().w;
                    if (img.size().h != img.size().w)
                    {
                        DEBUG_ERROR("non square tiles not supported: "
                                    << "cube face # " << i
                                    << "  tileSize : " << img.size().h << " , " << img.size().w);
                        return false;
                    }
                    tileSize = img.size().w;
                    faceSize = tileSize * tileDimensions;
                    cubefaces[i] = new Image(Size2D(faceSize, faceSize));
                    DEBUG_DEBUG("cube face # " << i << "  tileSize : " << tileSize
                                               << "  faceSize: " << faceSize);
                }

                if (img.size().w != tileSize)
                {
                    // jpeg image size doesn't correspond to tile size
                    DEBUG_ERROR("JPEG size != tile size, tile # " << cChunk);
                    return false;
                }

                ////////////////////////////////////////
                //   copy to cube face

                int h = t % tileDimensions;
                int v = t / tileDimensions;

                int left = h * tileSize;
                //int right	= left + tileSize;

                int top = v * tileSize;
                //int bottom	= top + tileSize;

                unsigned char *srcPtr = img.getData();
                unsigned char *destPtr = cubefaces[i]->getData() + 3 * left + 3 * faceSize * top;

                for (int y = 0; y < tileSize; y++)
                {
                    memcpy(destPtr, srcPtr, 3 * tileSize);
                    destPtr += 3 * faceSize;
                    srcPtr += 3 * tileSize;
                }
            }
            /*
#ifdef DEBUG
        {
            std::string t;
            FPV_S2S(t, "fpv_dbg_face_" << i << ".ppm");
            cubefaces[i]->writePPM(t);
        }
#endif
        */
        }
        return true;
    }

    /********************* SEEK AND EXTRACT IMAGES CYLINDER ***************************/
    //
    // Seek and extract the image from a cylindrical pano
    //

    bool QTVRDecoder::extractCylImage(Image *&img)
    {
        //long  count;
        //    int     i;

        if (m_type != Parameters::PANO_CYLINDRICAL)
        {
            m_error = "not a cylindrical panorama";
            return false;
        }

        /* SEE IF WE NEED TO SPECIAL-CASE TILED IMAGES */
        if (gImagesAreTiled)
        {
            return SeekAndExtractImagesCyl_Tiled(img);
        }

        //printf("\n\n_______SEEK & EXTRACT IMAGES   CYL_______\n\n");

        if (!gFoundJPEGs)
        {
            printf("No usable JPEG images were found\n");
            return false;
        }

        DEBUG_DEBUG("extracting single cylinder image  chunk: " << gVideoChunkOffset[0]);
        DEBUG_DEBUG("cylinder orientation: " << (m_horizontalCyl ? "horizontal" : "vertical"));
        /***************************/
        /* SEEK TO THE JPEG'S DATA */
        /***************************/

        fseek(gFile, gVideoChunkOffset[0], SEEK_SET);

        img = new Image;
        // decode jpeg cube face, and rotate 90 CW, if required
        if (!decodeJPEG(gFile, *img, (!m_horizontalCyl)))
        {
            m_error = "JPEG decoding failed";
            DEBUG_ERROR(m_error);
            if (img)
            {
                delete img;
            }
            return false;
        }

        return true;
    }

    /********************* SEEK AND EXTRACT IMAGES:  TILED ***************************/

    bool QTVRDecoder::SeekAndExtractImagesCyl_Tiled(Image *&image)
    {
        //int     i=0;
        //int     tileDimensions; //, compSize;
        //int     faceSize;

        //printf("\n\n_______SEEK & EXTRACT TILED IMAGES_______\n\n");

        //tileDimensions = (int) sqrt((float)gNumTilesPerImage);

        //printf("tileDimensions = %d\n", tileDimensions);

        // init tile assembly
        if (image)
        {
            delete image;
        }
        image = 0;
        Size2D tileSize;

        // load and assemble tiles.
        for (int t = 0; t < gNumTilesPerImage; t++)
        {

            //printf("Processing tile #%d...\n", t);
            DEBUG_DEBUG(" tile: " << t << "  offset: " << gVideoChunkOffset[t]);
            /* SEEK TO THE JPEG'S DATA */

            fseek(gFile, gVideoChunkOffset[t], SEEK_SET);
            if (ferror(gFile))
            {
                printf("LoadTilesForFace:  fseek failed!\n");
                continue;
            }

            /* DECODE IT */

            Image tile;
            // decode jpg
            if (!decodeJPEG(gFile, tile, (!m_horizontalCyl)))
            {
                m_error = "JPEG decoding failed";
                DEBUG_ERROR(m_error);
                return false;
            }

            /*
#ifdef DEBUG
        {
            std::string tfn;
            FPV_S2S(tfn, "fpv_dbg_stripe_" << i <<  "_tile_" << t  << ".pnm");
            tile.writePPM(tfn);
        }
#endif
        */
            /* CALCULATE THE DIMENSIONS OF THE TARGET IMAGE */
            if (image == 0)
            {
                // resize cube face
                tileSize = tile.size();

                Size2D imgSize(tileSize.w * gNumTilesPerImage, tileSize.h);
                image = new Image(imgSize);
                DEBUG_DEBUG("image tileSize : " << tileSize.w << "x" << tileSize.h
                                                << "  imgSize: " << imgSize.w << "x" << imgSize.h);
            }
            if (tile.size().w != tileSize.w || tile.size().h != tileSize.h)
            {
                // jpeg image size doesn't correspond to tile size
                m_error = "Tiles with different size found";
                DEBUG_ERROR(m_error);
                return false;
            }

            ////////////////////////////////////////
            //   add tile to image

            int left = 0;
            int right = 0;
            if (m_horizontalCyl)
            {
                left = t * tileSize.w;
                right = left + tileSize.w;
            }
            else
            {
                left = image->size().w - (t + 1) * tileSize.w;
                right = left + tileSize.w;
            }

            unsigned char *srcPtr = tile.getData();
            unsigned char *destPtr = image->getData() + 3 * left;

            for (int y = 0; y < tileSize.h; y++)
            {
                memcpy(destPtr, srcPtr, 3 * tileSize.w);
                destPtr += image->getRowStride();
                srcPtr += tile.getRowStride();
            }
            /*
#ifdef DEBUG
        {
            std::string t;
            FPV_S2S(t, "fpv_dbg_img_" << i << ".ppm");
            image->writePPM(t);
        }
#endif
        */
        }
        return true;
    }

    /**************************** SWIZZLE INT ***********************************/
    //
    // Converts a 4-byte int to reverse the Big-Little Endian order of the bytes.
    //
    // Quicktime movies are in BigEndian format, so when reading on a PC or other Little-Endian
    // machine, we've got to swap the bytes around.
    //

    void QTVRDecoder::Swizzle(uint32 *value)
    {
        Swizzle((int32 *)value);
    }

    void QTVRDecoder::Swizzle(int32 *value)
    {
        if (!m_HostBigEndian)
        {
            char *n, b1, b2, b3, b4;
            n = (char *)value;

            b1 = n[0]; // get the 4 bytes
            b2 = n[1];
            b3 = n[2];
            b4 = n[3];

            n[0] = b4; // save in reverse order
            n[1] = b3;
            n[2] = b2;
            n[3] = b1;
        }
    }

    // convert 16 bit values to native byteorder
    void QTVRDecoder::Swizzle(uint16 *value)
    {
        Swizzle((int16 *)value);
    }

    void QTVRDecoder::Swizzle(int16 *value)
    {
        if (!m_HostBigEndian)
        {
            char *n, b1, b2;
            n = (char *)value;

            b1 = n[0]; // get the 4 bytes
            b2 = n[1];

            n[0] = b2; // save in reverse order
            n[1] = b1;
        }
    }

} // namespace FPV
