// Compiler implementation of the D programming language
// Copyright (c) 2012-2012 by Digital Mars
// All Rights Reserved
// written by Walter Bright
// http://www.digitalmars.com
// License for redistribution is by either the Artistic License
// in artistic.txt, or the GNU General Public License in gnu.txt.
// See the included readme.txt for details.

// This module generates the .debug$S and .debug$T sections for Win64,
// which are the MS-Coff symbolic debug info and type debug info sections.

#if !SPP

#include        <stdio.h>
#include        <string.h>
#include        <stdlib.h>
#include        <time.h>

#include        "cc.h"
#include        "el.h"
#include        "code.h"
#include        "oper.h"
#include        "global.h"
#include        "type.h"
#include        "dt.h"
#include        "exh.h"
#include        "cgcv.h"
#include        "cv4.h"
#include        "obj.h"
#include        "outbuf.h"

static char __file__[] = __FILE__;      /* for tassert.h                */
#include        "tassert.h"

#if _MSC_VER || __sun
#include        <alloca.h>
#endif

#if MARS
#if TARGET_WINDOS

// The "F1" section, which is the symbols
static Outbuffer *F1_buf;

// The "F2" section, which is the line numbers
static Outbuffer *F2_buf;

// The "F3" section, which is global and a string table of source file names.
static Outbuffer *F3_buf;

// The "F4" section, which is global and a lists info about source files.
static Outbuffer *F4_buf;

/* Fixups that go into F1 section
 */
struct F1_Fixups
{
    Symbol *s;
    unsigned offset;
};

static Outbuffer *F1fixup;      // array of F1_Fixups

/* Struct in which to collect per-function data, for later emission
 * into .debug$S.
 */
struct FuncData
{
    Symbol *sfunc;
    unsigned section_length;
    const char *srcfilename;
    unsigned srcfileoff;
    unsigned linepairstart;     // starting index of offset/line pairs in linebuf[]
    unsigned linepairnum;       // number of offset/line pairs
    Outbuffer *f1buf;
    Outbuffer *f1fixup;
};

FuncData currentfuncdata;

static Outbuffer *funcdata;     // array of FuncData's

static Outbuffer *linepair;     // array of offset/line pairs

unsigned cv8_addfile(const char *filename);
void cv8_writesection(int seg, unsigned type, Outbuffer *buf);

/************************************************
 * Called at the start of an object file generation.
 * One source file can generate multiple object files; this starts an object file.
 * Input:
 *      filename        source file name
 */
void cv8_initfile(const char *filename)
{
    //printf("cv8_initfile()\n");

    // Recycle buffers; much faster than delete/renew

    if (!F1_buf)
        F1_buf = new Outbuffer(1024);
    F1_buf->setsize(0);

    if (!F1fixup)
        F1fixup = new Outbuffer(1024);
    F1fixup->setsize(0);

    if (!F2_buf)
        F2_buf = new Outbuffer(1024);
    F2_buf->setsize(0);

    if (!F3_buf)
        F3_buf = new Outbuffer(1024);
    F3_buf->setsize(0);
    F3_buf->writeByte(0);       // first "filename"

    if (!F4_buf)
        F4_buf = new Outbuffer(1024);
    F4_buf->setsize(0);

    if (!funcdata)
        funcdata = new Outbuffer(1024);
    funcdata->setsize(0);

    if (!linepair)
        linepair = new Outbuffer(1024);
    linepair->setsize(0);

    memset(&currentfuncdata, 0, sizeof(currentfuncdata));

    cv_init();
}

void cv8_termfile(const char *objfilename)
{
    //printf("cv8_termfile()\n");

    /* Write out the debug info sections.
     */

    int seg = MsCoffObj::seg_debugS();

    unsigned v = 4;
    objmod->bytes(seg,0,4,&v);

    /* Start with starting symbol in separate "F1" section
     */
    Outbuffer buf(1024);
    size_t len = strlen(objfilename);
    buf.writeWord(2 + 4 + len + 1);
    buf.writeWord(S_COMPILAND_V3);
    buf.write32(0);
    buf.write(objfilename, len + 1);
    cv8_writesection(seg, 0xF1, &buf);

    // Write out "F2" sections
    unsigned length = funcdata->size();
    unsigned char *p = funcdata->buf;
    for (unsigned u = 0; u < length; u += sizeof(FuncData))
    {   FuncData *fd = (FuncData *)(p + u);

        F2_buf->setsize(0);

        F2_buf->write32(fd->sfunc->Soffset);
        F2_buf->write32(0);
        F2_buf->write32(fd->section_length);
        F2_buf->write32(fd->srcfileoff);
        F2_buf->write32(fd->linepairnum);
        F2_buf->write32(fd->linepairnum * 8 + 12);
        F2_buf->write(linepair->buf + fd->linepairstart * 8, fd->linepairnum * 8);

        int f2seg = seg;
        if (symbol_iscomdat(fd->sfunc))
        {
            f2seg = MsCoffObj::seg_debugS_comdat(fd->sfunc);
            objmod->bytes(f2seg,0,4,&v);
        }

        unsigned offset = SegData[f2seg]->SDoffset + 8;
        cv8_writesection(f2seg, 0xF2, F2_buf);
        objmod->reftoident(f2seg, offset, fd->sfunc, 0, CFseg | CFoff);

        if (f2seg != seg && fd->f1buf->size())
        {
            // Write out "F1" section
            unsigned f1offset = SegData[f2seg]->SDoffset;
            cv8_writesection(f2seg, 0xF1, fd->f1buf);

            // Fixups for "F1" section
            length = fd->f1fixup->size();
            p = fd->f1fixup->buf;
            for (unsigned u = 0; u < length; u += sizeof(F1_Fixups))
            {   F1_Fixups *f = (F1_Fixups *)(p + u);

                objmod->reftoident(f2seg, f1offset + 8 + f->offset, f->s, 0, CFseg | CFoff);
            }
        }
    }

    // Write out "F3" section
    cv8_writesection(seg, 0xF3, F3_buf);

    // Write out "F4" section
    cv8_writesection(seg, 0xF4, F4_buf);

    if (F1_buf->size())
    {
        // Write out "F1" section
        unsigned f1offset = SegData[seg]->SDoffset;
        cv8_writesection(seg, 0xF1, F1_buf);

        // Fixups for "F1" section
        length = F1fixup->size();
        p = F1fixup->buf;
        for (unsigned u = 0; u < length; u += sizeof(F1_Fixups))
        {   F1_Fixups *f = (F1_Fixups *)(p + u);

            objmod->reftoident(seg, f1offset + 8 + f->offset, f->s, 0, CFseg | CFoff);
        }
    }

    // Write out .debug$T section
    cv_term();
}

/************************************************
 * Called at the start of a module.
 * Note that there can be multiple modules in one object file.
 * cv8_initfile() must be called first.
 */
void cv8_initmodule(const char *filename, const char *modulename)
{
    //printf("cv8_initmodule(filename = %s, modulename = %s)\n", filename, modulename);

    /* Experiments show that filename doesn't have to be qualified if
     * it is relative to the directory the .exe file is in.
     */
    currentfuncdata.srcfileoff = cv8_addfile(filename);
}

void cv8_termmodule()
{
    //printf("cv8_termmodule()\n");
    assert(config.exe == EX_WIN64);
}

/******************************************
 * Called at the start of a function.
 */
void cv8_func_start(Symbol *sfunc)
{
    //printf("cv8_func_start(%s)\n", sfunc->Sident);
    currentfuncdata.sfunc = sfunc;
    currentfuncdata.section_length = 0;
    currentfuncdata.srcfilename = NULL;
    currentfuncdata.srcfileoff = 0;
    currentfuncdata.linepairstart += currentfuncdata.linepairnum;
    currentfuncdata.linepairnum = 0;
    currentfuncdata.f1buf = F1_buf;
    currentfuncdata.f1fixup = F1fixup;
    if (symbol_iscomdat(sfunc))
    {
        currentfuncdata.f1buf = new Outbuffer(128);
        currentfuncdata.f1fixup = new Outbuffer(128);
    }
}

void cv8_func_term(Symbol *sfunc)
{
    //printf("cv8_func_term(%s)\n", sfunc->Sident);

    assert(currentfuncdata.sfunc == sfunc);
    currentfuncdata.section_length = retoffset + retsize;

    funcdata->write(&currentfuncdata, sizeof(currentfuncdata));

    // Write function symbol
    idx_t typidx = cv_typidx(sfunc->Stype);
    const char *id = sfunc->prettyIdent ? sfunc->prettyIdent : prettyident(sfunc);
    size_t len = strlen(id);
    /*
     *  2       length (not including these 2 bytes)
     *  2       S_GPROC_V3
     *  4       parent
     *  4       pend
     *  4       pnext
     *  4       size of function
     *  4       size of function prolog
     *  4       offset to function epilog
     *  4       type index
     *  6       seg:offset of function start
     *  1       flags
     *  n       0 terminated name string
     */
    Outbuffer *buf = currentfuncdata.f1buf;
    buf->reserve(2 + 2 + 4 * 7 + 6 + 1 + len + 1);
    buf->writeWordn( 2 + 4 * 7 + 6 + 1 + len + 1);
    buf->writeWordn(sfunc->Sclass == SCstatic ? S_LPROC_V3 : S_GPROC_V3);
    buf->write32(0);            // parent
    buf->write32(0);            // pend
    buf->write32(0);            // pnext
    buf->write32(currentfuncdata.section_length);       // size of function
    buf->write32(startoffset);          // size of prolog
    buf->write32(retoffset);                    // offset to epilog
    buf->write32(typidx);

    F1_Fixups f1f;
    f1f.s = sfunc;
    f1f.offset = buf->size();
    currentfuncdata.f1fixup->write(&f1f, sizeof(f1f));
    buf->write32(0);
    buf->writeWordn(0);

    buf->writeByte(0);
    buf->writen(id, len + 1);

    // Write function end symbol
    buf->writeWord(2);
    buf->writeWord(S_END);

    currentfuncdata.f1buf = F1_buf;
    currentfuncdata.f1fixup = F1fixup;
}

/**********************************************
 */

void cv8_linnum(Srcpos srcpos, targ_size_t offset)
{
    //printf("cv8_linnum(file = %s, line = %d, offset = x%x)\n", srcpos.Sfilename, (int)srcpos.Slinnum, (unsigned)offset);
    if (currentfuncdata.srcfilename)
    {
        /* Ignore line numbers from different files in the same function.
         * This can happen with inlined functions.
         * To make this work would require a separate F2 section for each different file.
         */
        if (currentfuncdata.srcfilename != srcpos.Sfilename &&
            strcmp(currentfuncdata.srcfilename, srcpos.Sfilename))
            return;
    }
    else
    {
        currentfuncdata.srcfilename = srcpos.Sfilename;
        currentfuncdata.srcfileoff  = cv8_addfile(srcpos.Sfilename);
    }
    linepair->write32((unsigned)offset);
    linepair->write32((unsigned)srcpos.Slinnum | 0x80000000);
    ++currentfuncdata.linepairnum;
}

/**********************************************
 * Add source file, if it isn't already there.
 * Return offset into F4.
 */

unsigned cv8_addfile(const char *filename)
{
    //printf("cv8_addfile('%s')\n", filename);

    /* The algorithms here use a linear search. This is acceptable only
     * because we expect only 1 or 2 files to appear.
     * Unlike C, there won't be lots of .h source files to be accounted for.
     */

    unsigned length = F3_buf->size();
    unsigned char *p = F3_buf->buf;
    size_t len = strlen(filename);

    unsigned off = 1;
    while (off + len < length)
    {
        if (memcmp(p + off, filename, len + 1) == 0)
        {   // Already there
            //printf("\talready there at %x\n", off);
            goto L1;
        }
        off += strlen((const char *)(p + off)) + 1;
    }
    off = length;
    // Add it
    F3_buf->write(filename, len + 1);

L1:
    // off is the offset of the filename in F3.
    // Find it in F4.

    length = F4_buf->size();
    p = F4_buf->buf;

    unsigned u = 0;
    while (u + 8 <= length)
    {
        //printf("\t%x\n", *(unsigned *)(p + u));
        if (off == *(unsigned *)(p + u))
        {
            //printf("\tfound %x\n", u);
            return u;
        }
        u += 4;
        unsigned short type = *(unsigned short *)(p + u);
        u += 2;
        if (type == 0x0110)
            u += 16;            // MD5 checksum
        u += 2;
    }

    // Not there. Add it.
    F4_buf->write32(off);

    /* Write 10 01 [MD5 checksum]
     *   or
     * 00 00
     */
    F4_buf->writeShort(0);

    // 2 bytes of pad
    F4_buf->writeShort(0);

    //printf("\tadded %x\n", length);
    return length;
}

void cv8_writesection(int seg, unsigned type, Outbuffer *buf)
{
    /* Write out as:
     *  bytes   desc
     *  -------+----
     *  4       type
     *  4       length
     *  length  data
     *  pad     pad to 4 byte boundary
     */
    unsigned off = SegData[seg]->SDoffset;
    objmod->bytes(seg,off,4,&type);
    unsigned length = buf->size();
    objmod->bytes(seg,off+4,4,&length);
    objmod->bytes(seg,off+8,length,buf->buf);
    // Align to 4
    unsigned pad = ((length + 3) & ~3) - length;
    objmod->lidata(seg,off+8+length,pad);
}

void cv8_outsym(Symbol *s)
{
    //printf("cv8_outsym(s = '%s')\n", s->Sident);
    //symbol_print(s);
    if (s->Sflags & SFLnodebug)
        return;
return;

    idx_t typidx = cv_typidx(s->Stype);
    const char *id = s->prettyIdent ? s->prettyIdent : prettyident(s);
    size_t len = strlen(id);

    F1_Fixups f1f;

    switch (s->Sclass)
    {
        case SCglobal:
            /*
             *  2       length (not including these 2 bytes)
             *  2       S_GDATA_V3
             *  4       typidx
             *  6       ref to symbol
             *  n       0 terminated name string
             */
            F1_buf->reserve(2 + 2 + 4 + 6 + len + 1);
            F1_buf->writeWordn(2 + 4 + 6 + len + 1);
            F1_buf->writeWordn(S_GDATA_V3);
            F1_buf->write32(typidx);

            f1f.s = s;
            f1f.offset = F1_buf->size();
            F1fixup->write(&f1f, sizeof(f1f));
            F1_buf->write32(0);
            F1_buf->writeWordn(0);

            F1_buf->writen(id, len + 1);
            break;
    }
}

#endif
#endif
#endif