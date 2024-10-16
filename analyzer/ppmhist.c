/* ppmhist.c - read a PPM image and compute a color histogram
**
** Copyright (C) 1989 by Jef Poskanzer.
**
** Permission to use, copy, modify, and distribute this software and its
** documentation for any purpose and without fee is hereby granted, provided
** that the above copyright notice appear in all copies and that both that
** copyright notice and this permission notice appear in supporting
** documentation.  This software is provided "as is" without express or
** implied warranty.
*/

#include <assert.h>

#include "pm_c_util.h"
#include "mallocvar.h"
#include "nstring.h"
#include "shhopt.h"
#include "ppm.h"

enum sort {SORT_BY_FREQUENCY, SORT_BY_RGB};

enum ColorFmt {FMT_DECIMAL, FMT_HEX, FMT_FLOAT, FMT_PPMPLAIN};

struct CmdlineInfo {
    /* All the information the user supplied in the command line,
       in a form easy for the program to use.
    */
    const char * inputFileName;  /* Name of input file */
    unsigned int noheader;
    enum ColorFmt colorFmt;
    unsigned int colorname;
    enum sort sort;
    unsigned int forensic;
};



static void
parseCommandLine(int argc, const char ** argv,
                 struct CmdlineInfo * const cmdlineP) {
/*----------------------------------------------------------------------------
   Note that the file spec array we return is stored in the storage that
   was passed to us as the argv array.
-----------------------------------------------------------------------------*/
    optStruct3 opt;  /* set by OPTENT3 */
    optEntry * option_def;
    unsigned int option_def_index;

    unsigned int hexcolorOpt, floatOpt, mapOpt, nomapOpt;
    const char * sort_type;

    MALLOCARRAY(option_def, 100);

    option_def_index = 0;   /* incremented by OPTENTRY */
    OPTENT3(0,   "map",       OPT_FLAG, NULL,  &mapOpt,                0);
    OPTENT3(0,   "nomap",     OPT_FLAG, NULL,  &nomapOpt,              0);
    OPTENT3(0,   "noheader",  OPT_FLAG, NULL,  &cmdlineP->noheader,    0);
    OPTENT3(0,   "hexcolor",  OPT_FLAG, NULL,  &hexcolorOpt,           0);
    OPTENT3(0,   "float",     OPT_FLAG, NULL,  &floatOpt,              0);
    OPTENT3(0,   "colorname", OPT_FLAG, NULL,  &cmdlineP->colorname,   0);
    OPTENT3(0,   "sort",      OPT_STRING, &sort_type, NULL,            0);
    OPTENT3(0,   "forensic",  OPT_FLAG, NULL,  &cmdlineP->forensic,    0);

    opt.opt_table = option_def;
    opt.short_allowed = FALSE;  /* We have no short (old-fashioned) options */
    opt.allowNegNum = FALSE;  /* We have no parms that are negative numbers */

    /* Set defaults */
    sort_type = "frequency";

    pm_optParseOptions4(&argc, argv, opt, sizeof(opt), 0);
        /* Uses and sets argc, argv, and some of *cmdlineP and others. */
    free(option_def);

    if (argc-1 == 0)
        cmdlineP->inputFileName = "-";
    else if (argc-1 != 1)
        pm_error("Program takes zero or one argument (filename).  You "
                 "specified %d", argc-1);
    else
        cmdlineP->inputFileName = argv[1];

    if (hexcolorOpt + floatOpt + mapOpt > 1)
        pm_error("You can specify only one of -hexcolor, -float, and -map");
    if (hexcolorOpt)
        cmdlineP->colorFmt = FMT_HEX;
    else if (floatOpt)
        cmdlineP->colorFmt = FMT_FLOAT;
    else if (mapOpt) {
        if (cmdlineP->forensic)
            pm_error("You cannot specify -map and -forensic together");

        cmdlineP->colorFmt = FMT_PPMPLAIN;
    } else
        cmdlineP->colorFmt = FMT_DECIMAL;

    if (strcmp(sort_type, "frequency") == 0)
        cmdlineP->sort = SORT_BY_FREQUENCY;
    else if (strcmp(sort_type, "rgb") == 0)
        cmdlineP->sort = SORT_BY_RGB;
    else
        pm_error("Invalid -sort value: '%s'.  The valid values are "
                 "'frequency' and 'rgb'.", sort_type);
}



static int
cmpUint(unsigned int const a,
        unsigned int const b) {
/*----------------------------------------------------------------------------
   Return 1 if 'a' > 'b'; -1 if 'a' is < 'b'; 0 if 'a' == 'b'.

   I.e. what a libc qsort() comparison function returns.
-----------------------------------------------------------------------------*/
    return a > b ? 1 : a < b ? -1 : 0;
}



#ifndef LITERAL_FN_DEF_MATCH
static qsort_comparison_fn rgbcompare;
#endif

static int
rgbcompare(const void * const a,
           const void * const b) {
/*----------------------------------------------------------------------------
   This is a 'qsort' collation function.
-----------------------------------------------------------------------------*/
    const struct colorhist_item * const histItem1P = a;
    const struct colorhist_item * const histItem2P = b;

    int retval;

    retval = cmpUint(PPM_GETR(histItem1P->color),
                     PPM_GETR(histItem2P->color));
    if (retval == 0) {
        retval = cmpUint(PPM_GETG(histItem1P->color),
                         PPM_GETG(histItem2P->color));
        if (retval == 0)
            retval = cmpUint(PPM_GETB(histItem1P->color),
                             PPM_GETB(histItem2P->color));
    }
    return retval;
}



#ifndef LITERAL_FN_DEF_MATCH
static qsort_comparison_fn countcompare;
#endif


static int
countcompare(const void * const a,
             const void * const b) {
/*----------------------------------------------------------------------------
   This is a 'qsort' collation function.
-----------------------------------------------------------------------------*/
    const struct colorhist_item * const histItem1P = a;
    const struct colorhist_item * const histItem2P = b;

    int const countComparison = cmpUint(histItem2P->value, histItem1P->value);

    if (countComparison == 0) {
        /* Counts are the same; use RGB secondary sort */
        return rgbcompare(a, b);
    } else
        return countComparison;
}



static pixval
universalMaxval(pixval const maxval,
                int    const format) {
/*----------------------------------------------------------------------------
  A maxval that makes it impossible for a pixel to be invalid in an image that
  states its maxval as 'maxval' and has format 'format'.

  E.g. in a one-byte-per-sample image, it's not possible to read a sample
  value greater than 255, so a maxval of 255 makes it impossible for a sample
  to be invalid.

  But: we never go above 65535, which means our maxval isn't entirely
  universal.  If the image is plain PPM, it could contain a pixel that
  exceeds even that.
-----------------------------------------------------------------------------*/
    assert(0 < maxval && maxval < 65536);

    if (format == RPPM_FORMAT || format == RPGM_FORMAT) {
        /*  Raw PPM stream has either one or two bytes per pixel, depending
            upon its stated maxval.
        */
        if (maxval > 255)
            return 65535;
        else
            return 255;
    } else if (format == RPBM_FORMAT) {
        /* A Raw PBM stream has one bit per pixel, which libnetpbm renders
           as 0 or 255 when we read it.
        */
        assert(maxval == 255);
        return 255;
    } else {
        /* A plain PPM stream has essentially unlimited range in the
           tokens that are supposed to be sample values.  We arbitrarily draw
           the line at 65535.
        */
        return 65535;
    }
}



static bool
colorIsValid(pixel  const color,
             pixval const maxval) {

    pixval const r = PPM_GETR(color);
    pixval const g = PPM_GETG(color);
    pixval const b = PPM_GETB(color);

    return r <= maxval && g <= maxval && b <= maxval;
}



static void
separateInvalidItems(colorhist_vector const chv,
                     colorhist_vector const chvInvalid,
                     pixval           const maxval,
                     unsigned int     const colorCt,
                     unsigned int  *  const validColorCtP) {
/*----------------------------------------------------------------------------
  Move invalid color entries from chv to chvInvalid.
  Count how many color entries are valid.
-----------------------------------------------------------------------------*/
    unsigned int i;
    unsigned int validCt;
    unsigned int invalidCt;

    for (i = 0, validCt = 0, invalidCt = 0; i < colorCt; ++i) {
        if (!colorIsValid(chv[i].color, maxval))
            chvInvalid[invalidCt++] = chv[i];
        else
            chv[validCt++] = chv[i];
    }
    *validColorCtP = validCt;
}



static void
sortHistogramForensic(enum sort        const sortFn,
                      colorhist_vector const chv,
                      colorhist_vector const chvInvalid,
                      pixval           const maxval,
                      unsigned int     const colorCt,
                      unsigned int *   const validColorCtP) {

    unsigned int validColorCt;

    separateInvalidItems(chv, chvInvalid, maxval, colorCt, &validColorCt);

    {
        int (*compare_function)(const void *, const void *);

        switch (sortFn) {
        case SORT_BY_FREQUENCY: compare_function = countcompare; break;
        case SORT_BY_RGB:       compare_function = rgbcompare;   break;
        }

        qsort((void*) chv, validColorCt,
              sizeof(struct colorhist_item), compare_function);

        qsort((void*) chvInvalid, colorCt - validColorCt,
              sizeof(struct colorhist_item), compare_function);
    }
    *validColorCtP = validColorCt;
}



static void
sortHistogramNormal(enum sort        const sortFn,
                    colorhist_vector const chv,
                    unsigned int     const colorCt) {

    int (*compare_function)(const void *, const void *);

    switch (sortFn) {
    case SORT_BY_FREQUENCY: compare_function = countcompare; break;
    case SORT_BY_RGB:       compare_function = rgbcompare;   break;
    }

    qsort((void*) chv, colorCt, sizeof(struct colorhist_item),
          compare_function);
}


typedef struct {
    unsigned int nTotal;
        /* Number of colors; sum of all the following */
    unsigned int nBlack;
        /* 1 if black is present; 0 otherwise */
    unsigned int nWhite;
        /* 1 if white is present; 0 otherwise */
    unsigned int nGray;
        /* Number of gray shades, not including black and white */
    unsigned int nColor;
        /* number of colors other than black, white, and gray */

} ColorSummary;



static ColorSummary
colorSummary(colorhist_vector const chv,
             unsigned int     const colorCt,
             pixval           const maxval) {

    ColorSummary retval;

    unsigned int i;

    retval.nTotal = colorCt;

    for (i = 0,
             retval.nBlack = 0,
             retval.nWhite = 0,
             retval.nGray = 0,
             retval.nColor = 0;
         i < colorCt;
         ++i) {

        pixel const color = chv[i].color;
        pixval const r = PPM_GETR(color);
        pixval const g = PPM_GETG(color);
        pixval const b = PPM_GETB(color);

        if (r == 0 && g == 0 && b == 0)
            ++retval.nBlack;
        else if (r == maxval && g == maxval && b == maxval)
            ++retval.nWhite;
        else if (r == g && r ==b)
            ++retval.nGray;
        else
            ++retval.nColor;
    }
    assert(retval.nBlack + retval.nWhite + retval.nGray + retval.nColor ==
           retval.nTotal);

    return retval;
}


static void
printColorSummary(ColorSummary const colorSummary,
                  const char * const prefix) {

    printf("%sSummary: %u colors: %u black, %u white, %u gray, %u color\n",
           prefix,
           colorSummary.nTotal,
           colorSummary.nBlack,
           colorSummary.nWhite,
           colorSummary.nGray,
           colorSummary.nColor);

    printf("\n");
}



static const char *
colornameLabel(pixel                 const color,
               pixval                const maxval,
               const ppm_ColorDict * const colorDictP) {
/*----------------------------------------------------------------------------
   Return the name of the color 'color' or the closest color in the
   dictionary to it.  If the name returned is not the exact color,
   prefix it with "*".  Otherwise, prefix it with " ".

   *colorDictP is the color dictionary.

   Return the name in static storage within this subroutine.
-----------------------------------------------------------------------------*/
    static char retval[32];
    int colorIndex;

    pixel color255;
        /* The color, normalized to a maxval of 255: the maxval of a color
           dictionary.
        */

    PPM_DEPTH(color255, color, maxval, 255);

    colorIndex = ppm_findclosestcolor(colorDictP->color, colorDictP->count,
                                      &color255);

    assert(colorIndex >= 0 && colorIndex < colorDictP->count);

    if (PPM_EQUAL(colorDictP->color[colorIndex], color))
        STRSCPY(retval, " ");
    else
        STRSCPY(retval, "*");

    STRSCAT(retval, colorDictP->name[colorIndex]);

    return retval;
}



static void
printColors(colorhist_vector      const chv,
            int                   const colorCt,
            pixval                const maxval,
            enum ColorFmt         const colorFmt,
            bool                  const withColorName,
            const ppm_ColorDict * const colorDictP) {
/*----------------------------------------------------------------------------
   Print to Standard Output the list of colors, one per line in 'chv',
   of which there are 'colorCt'.

   Print the color in format 'colorFmt'.

   If 'withColorName' is true, we add the name of each color to the line.
   'oclorDict' is a list of known names of colors. If the color is not in the
   list, we add the name of the color closest to it whose name we know,
   prefixed by "*".
-----------------------------------------------------------------------------*/
    unsigned int i;

    for (i = 0; i < colorCt; ++i) {
        pixval       const r          = PPM_GETR(chv[i].color);
        pixval       const g          = PPM_GETG(chv[i].color);
        pixval       const b          = PPM_GETB(chv[i].color);
        double       const lum        = PPM_LUMIN(chv[i].color);
        unsigned int const intLum     = lum + 0.5;
        double       const floatLum   = lum / maxval;
        unsigned int const count      = chv[i].value;

        const char * colornameValue;

        if (withColorName)
            colornameValue = colornameLabel(chv[i].color, maxval, colorDictP);
        else
            colornameValue = "";

        switch(colorFmt) {
        case FMT_FLOAT:
            printf(" %1.3f %1.3f %1.3f\t%1.3f\t%7d %s\n",
                   (double)r / maxval,
                   (double)g / maxval,
                   (double)b / maxval,
                   floatLum, count, colornameValue);
            break;
        case FMT_HEX:
            printf("  %04x  %04x  %04x\t%5d\t%7d %s\n",
                   r, g, b, intLum, count, colornameValue);
            break;
        case FMT_DECIMAL:
            printf(" %5d %5d %5d\t%5d\t%7d %s\n",
                   r, g, b, intLum, count, colornameValue);
            break;
        case FMT_PPMPLAIN:
            printf(" %5d %5d %5d#\t%5d\t%7d %s\n",
                   r, g, b, intLum, count, colornameValue);
            break;
        }
    }
}



static void
printHistogram(colorhist_vector const chv,
               unsigned int     const colorCt,
               pixval           const maxval,
               enum ColorFmt    const colorFmt,
               bool             const wantHeader,
               bool             const wantColorName) {

    ppm_ColorDict * colorDictP;

    if (colorFmt == FMT_PPMPLAIN)
        printf("P3\n# color map\n%d 1\n%d\n", colorCt, maxval);

    if (wantHeader) {
        const char * commentDelim = colorFmt == FMT_PPMPLAIN ? "#" : " ";

        printColorSummary(colorSummary(chv, colorCt, maxval), commentDelim);

        printf("%s  r     g     b   \t lum \t count  %s\n",
               commentDelim, wantColorName ? "name" : "");
        printf("%s----- ----- ----- \t-----\t------- %s\n",
               commentDelim, wantColorName ? "----" : "");
    }
    if (wantColorName) {
        bool const mustOpenTrue = TRUE;

        colorDictP = ppm_colorDict_new(NULL, mustOpenTrue);
    } else
        colorDictP = NULL;

    printColors(chv, colorCt, maxval,
                colorFmt, wantColorName, colorDictP);

    if (colorDictP)
        ppm_colorDict_destroy(colorDictP);
}



static void
summarizeInvalidPixels(unsigned long int const validPixelCt,
                       unsigned long int const invalidPixelCt,
                       pixval            const maxval) {
/*----------------------------------------------------------------------------
  Print total count of valid and invalid pixels, if there are any
  invalid ones.
-----------------------------------------------------------------------------*/
    if (invalidPixelCt > 0) {
        unsigned long int const totalPixelCt = validPixelCt + invalidPixelCt;

        printf("\n");
        printf("** Image stream contains invalid sample values "
               "(above maxval %u)\n", maxval);
        printf("** Valid sample values : %lu (%5.4g%%)\n",
               validPixelCt,   (float) validPixelCt   / totalPixelCt * 100.0);
        printf("** Invalid sample values : %lu (%5.4g%%)\n",
               invalidPixelCt, (float) invalidPixelCt / totalPixelCt * 100.0);
    }
}



static void
printInvalidSamples(colorhist_vector const chv,
                    colorhist_vector const chvInvalid,
                    int              const totalColorCt,
                    unsigned int     const validColorCt,
                    pixval           const maxval,
                    enum ColorFmt    const colorFmt) {

    unsigned int const invalidColorCt = totalColorCt - validColorCt;

    unsigned int i;
    unsigned long int validPixelCt;
    unsigned long int invalidPixelCt;

    for (i = 0, validPixelCt = 0; i < validColorCt; ++i)
        validPixelCt += chv[i].value;

    for (i = 0, invalidPixelCt = 0; i < invalidColorCt; ++i) {
        pixval       const r     = PPM_GETR(chvInvalid[i].color);
        pixval       const g     = PPM_GETG(chvInvalid[i].color);
        pixval       const b     = PPM_GETB(chvInvalid[i].color);
        unsigned int const count = chvInvalid[i].value;

        invalidPixelCt += chvInvalid[i].value;

        switch(colorFmt) {
        case FMT_FLOAT:
            printf(" %1.3f %1.3f %1.3f\t\t%7d\n",
                   (double)r / maxval,
                   (double)g / maxval,
                   (double)b / maxval,
                   count);
            break;
        case FMT_HEX:
            printf("  %04x  %04x  %04x\t\t%7d\n",
                   r, g, b, count);
            break;
        case FMT_DECIMAL:
            printf(" %5d %5d %5d\t\t%7d\n",
                   r, g, b, count);
            break;
        case FMT_PPMPLAIN:
            assert(false);
            break;
        }
    }

    summarizeInvalidPixels(validPixelCt, invalidPixelCt, maxval);
}



int
main(int argc, const char *argv[]) {

    struct CmdlineInfo cmdline;
    FILE * ifP;
    colorhist_vector chv;
    colorhist_vector chvInvalid;
    int rows, cols;
    pixval maxval;
    pixval mmaxval;
    int format;
    int colorCt;
    unsigned int validColorCt;

    pm_proginit(&argc, argv);

    parseCommandLine(argc, argv, &cmdline);

    ifP = pm_openr(cmdline.inputFileName);

    ppm_readppminit(ifP, &cols, &rows, &maxval, &format);

    mmaxval = cmdline.forensic ? universalMaxval(maxval, format) : maxval;

    chv = ppm_computecolorhist2(ifP, cols, rows, mmaxval, format, 0, &colorCt);

    pm_close(ifP);

    /* Sort and produce histogram. */
    if (cmdline.forensic) {
        MALLOCARRAY(chvInvalid, colorCt);
        if (chvInvalid == NULL)
            pm_error("out of memory generating histogram");

        sortHistogramForensic(cmdline.sort, chv, chvInvalid,
                              maxval, colorCt, &validColorCt);
    } else {
        chvInvalid = NULL;
        sortHistogramNormal(cmdline.sort, chv, colorCt);
        validColorCt = colorCt;
    }

    printHistogram(chv, validColorCt, maxval,
                   cmdline.colorFmt, !cmdline.noheader, cmdline.colorname);

    if (colorCt > validColorCt)
        printInvalidSamples(chv, chvInvalid, colorCt, validColorCt,
                            maxval, cmdline.colorFmt);

    ppm_freecolorhist(chv);

    if (chvInvalid)
        ppm_freecolorhist(chvInvalid);

    return 0;
}


