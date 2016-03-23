/*
 * HDMI 4K converter for Axiom BETA footage.
 * 
 * Copyright (C) 2013 a1ex
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "stdint.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "ctype.h"
#include "unistd.h"
#include "sys/stat.h"
#include "math.h"
#include "cmdoptions.h"
#include "patternnoise.h"
#include "ufraw_routines.h"

/* image data */
uint16_t* rgbA;     /* HDMI frame A (width x height x 3, PPM order) */
uint16_t* rgbB;     /* HDMI frame B */
uint16_t* raw;      /* Bayer raw image (double resolution) */
int width;
int height;

struct cmd_group options[] = {
    OPTION_GROUP_EOL
};

#define FAIL(fmt,...) { fprintf(stderr, "Error: "); fprintf(stderr, fmt, ## __VA_ARGS__); fprintf(stderr, "\n"); exit(1); }
#define CHECK(ok, fmt,...) { if (!(ok)) FAIL(fmt, ## __VA_ARGS__); }

#define MIN(a,b) \
   ({ __typeof__ ((a)+(b)) _a = (a); \
      __typeof__ ((a)+(b)) _b = (b); \
     _a < _b ? _a : _b; })

#define MAX(a,b) \
   ({ __typeof__ ((a)+(b)) _a = (a); \
      __typeof__ ((a)+(b)) _b = (b); \
     _a > _b ? _a : _b; })

#define ABS(a) \
   ({ __typeof__ (a) _a = (a); \
     _a > 0 ? _a : -_a; })

#define COERCE(x,lo,hi) MAX(MIN((x),(hi)),(lo))

#define COUNT(x)        ((int)(sizeof(x)/sizeof((x)[0])))

static int endswith(char* str, char* suffix)
{
    int l1 = strlen(str);
    int l2 = strlen(suffix);
    if (l1 < l2)
    {
        return 0;
    }
    if (strcmp(str + l1 - l2, suffix) == 0)
    {
        return 1;
    }
    return 0;
}

static void reverse_bytes_order(uint8_t* buf, int count)
{
    uint16_t* buf16 = (uint16_t*) buf;
    int i;
    for (i = 0; i < count/2; i++)
    {
        uint16_t x = buf16[i];
        buf[2*i+1] = x;
        buf[2*i] = x >> 8;
    }
}

/* 16-bit PPM, as converted by FFMPEG */
/* rgb is allocated by us */
static void read_ppm(char* filename, uint16_t** prgb)
{
    FILE* fp = fopen(filename, "rb");
    CHECK(fp, "could not open %s", filename);

    /* PGM read code from dcraw, adapted for PPM */
    int dim[3]={0,0,0}, comment=0, number=0, error=0, nd=0, c;

      if (fgetc(fp) != 'P' || fgetc(fp) != '6') error = 1;
      while (!error && nd < 3 && (c = fgetc(fp)) != EOF) {
        if (c == '#')  comment = 1;
        if (c == '\n') comment = 0;
        if (comment) continue;
        if (isdigit(c)) number = 1;
        if (number) {
          if (isdigit(c)) dim[nd] = dim[nd]*10 + c -'0';
          else if (isspace(c)) {
        number = 0;  nd++;
          } else error = 1;
        }
      }
    
    CHECK(!(error || nd < 3), "not a valid PGM file\n");

    width = dim[0];
    height = dim[1];

    CHECK(prgb && !*prgb, "prgb");
    uint16_t* rgb = malloc(width * height * 2 * 3);
    *prgb = rgb;
    
    int size = fread(rgb, 1, width * height * 2 * 3, fp);
    CHECK(size == width * height * 2 * 3, "fread");
    fclose(fp);
    
    /* PPM is big endian, need to reverse it */
    reverse_bytes_order((void*)rgb, width * height * 2 * 3);
}

/* Output 16-bit PGM file */
/* caveat: it reverses bytes order in the buffer */
static void write_pgm(char* filename, uint16_t * raw, int w, int h)
{
    printf("Writing %s...\n", filename);
    FILE* f = fopen(filename, "wb");
    fprintf(f, "P5\n%d %d\n65535\n", w, h);

    /* PPM is big endian, need to reverse it */
    reverse_bytes_order((void*)raw, w * h * 2);
    fwrite(raw, 1, w * h * 2, f);
    fclose(f);
}

static int file_exists(char * filename)
{
    struct stat buffer;   
    return (stat (filename, &buffer) == 0);
}

static int file_exists_warn(char * filename)
{
    int ans = file_exists(filename);
    if (!ans) printf("Not found   : %s\n", filename);
    return ans;
}

static void convert_to_linear(uint16_t * raw, int w, int h)
{
    double gamma = 0.52;
    double gain = 0.85;
    double offset = 45;
    
    int* lut = malloc(0x10000 * sizeof(lut[0]));
    
    for (int i = 0; i < 0x10000; i++)
    {
        double data = i / 65535.0;
        
        /* undo HDMI 16-235 scaling */
        data = data * (235.0 - 16.0) / 255.0 + 16.0 / 255.0;
        
        /* undo gamma applied from our camera, before recording */
        data = pow(data, 1/gamma);
        
        /* scale the (now linear) values to cover the full 12-bit range,
         * with a black level of 128 */
        lut[i] = COERCE(data * 4095 / gain + 128 - offset, 0, 4095);
    }
    
    for (int i = 0; i < w * h; i++)
    {
        raw[i] = lut[raw[i]];
    }
    
    free(lut);
}

/**
 * Filters to recover Bayer channels (R,G1,G2,B) from two HDMI frames:
 * rgbA: R,G1,B
 * rgbB: R',G2,B' (delayed by 1 stop)
 * 
 * The exact placement of pixels might differ.
 * The filters will take care of any differences that may appear,
 * for example, the line swapping bug, which is still present - ping Herbert.
 * 
 * The black box (recorder or ffmpeg or maybe both) also applies a color matrix
 * to the raw data, which will be undone by these filters as well.
 * 
 * These filters were designed to be applied before linearization,
 * on Shogun footage transcoded with ffmpeg -vcodec copy 
 * (output is different with unprocessed MOVs - ffmpeg bug?)
 * 
 * Indices:
 *  - Bayer channel: x%2 + 2*(y%2),
 *  - column parity in the HDMI image: (x/2) % 2,
 *  - HDMI frame parity (A,B),
 *  - predictor channel (R,G,B),
 *  - filter kernel indices (i,j).
 */
const int filters[4][2][2][3][3][3] = {
  /* Red: */
  [2] = {
    /* even columns: */
    {
      /* from frame A: */
      {
        /* from red (sum=0.08): */
        {
          {    464,  -524,  -122 },
          {   -326,   951,   387 },
          {   -104,    21,   -83 },
        },
        /* from green (sum=0.00): */
        {
          {  -8348,  8083,   -79 },
          {   4527, -3959,   230 },
          {  -3018,  2623,   -30 },
        },
        /* from blue (sum=-0.03): */
        {
          {   7664, -7586,   -30 },
          {  -3884,  3730,    18 },
          {   2734, -2820,   -44 },
        },
      },
      /* from frame B: */
      {
        /* from red (sum=1.01): */
        {
          {   9398, -8421,   -99 },
          {   1177,  5070,   266 },
          {  -3032,  3936,   -42 },
        },
        /* from green (sum=-0.08): */
        {
          {  -9194,  8993,   -11 },
          {  -1766,  1417,   -21 },
          {   2541, -2394,  -190 },
        },
        /* from blue (sum=0.01): */
        {
          {   -347,   259,    90 },
          {    802,  -711,   -37 },
          {    498,  -359,   -93 },
        },
      },
    },
    /* odd columns: */
    {
      /* from frame A: */
      {
        /* from red (sum=0.84): */
        {
          {   -165,  -194,   866 },
          {    192,   263,  5460 },
          {   -149,   -32,   654 },
        },
        /* from green (sum=-0.12): */
        {
          {   -221,  5416, -5716 },
          {    263, 13406,-13528 },
          {   -175, -3649,  3261 },
        },
        /* from blue (sum=-0.01): */
        {
          {     44, -5604,  5608 },
          {     43,-13116, 12935 },
          {    -85,  3325, -3198 },
        },
      },
      /* from frame B: */
      {
        /* from red (sum=0.25): */
        {
          {    215, -2989,  2957 },
          {    526,  8633, -7560 },
          {    144,  1149, -1040 },
        },
        /* from green (sum=0.04): */
        {
          {     29,  2515, -2704 },
          {   -353, -7354,  8103 },
          {    -62,  -601,   763 },
        },
        /* from blue (sum=-0.01): */
        {
          {     -1,   256,  -216 },
          {    -76,   -43,   -64 },
          {     21,  -276,   332 },
        },
      },
    },
  },
  /* Green1: */
  [3] = {
    /* even columns: */
    {
      /* from frame A: */
      {
        /* from red (sum=0.07): */
        {
          {     65,     4,   -66 },
          {    590,  -145,   169 },
          {   -257,   335,  -110 },
        },
        /* from green (sum=0.75): */
        {
          {   5358, -5380,  -148 },
          {  -3296,  9438,   248 },
          {   -291,   344,  -118 },
        },
        /* from blue (sum=0.15): */
        {
          {  -5643,  5667,   -59 },
          {   3132, -2045,   105 },
          {    366,  -371,   101 },
        },
      },
      /* from frame B: */
      {
        /* from red (sum=0.03): */
        {
          {   -359,   326,   -96 },
          {  -6351,  6546,   148 },
          {   3276, -3338,    58 },
        },
        /* from green (sum=0.09): */
        {
          {    524,  -416,    40 },
          {   6370, -6356,   287 },
          {  -3268,  3512,    85 },
        },
        /* from blue (sum=-0.10): */
        {
          {   -236,   128,    31 },
          {   -493,     3,   -92 },
          {   -128,    82,   -73 },
        },
      },
    },
    /* odd columns: */
    {
      /* from frame A: */
      {
        /* from red (sum=0.33): */
        {
          {      3,  -212,   158 },
          {   -100, -1503,  4434 },
          {     63,  -172,    35 },
        },
        /* from green (sum=0.73): */
        {
          {    -16,  5670, -5584 },
          {    217, 20874,-15239 },
          {    -30, -3319,  3400 },
        },
        /* from blue (sum=-0.01): */
        {
          {     22, -4978,  5059 },
          {    140,-11578, 11114 },
          {    -23,  3964, -3788 },
        },
      },
      /* from frame B: */
      {
        /* from red (sum=-0.23): */
        {
          {   -316,  7759, -7823 },
          {  -1557,  4812, -4535 },
          {   -340,   197,  -104 },
        },
        /* from green (sum=0.12): */
        {
          {     38, -7236,  7264 },
          {    107, -4937,  5463 },
          {   -133,  -218,   607 },
        },
        /* from blue (sum=0.07): */
        {
          {     -4,  -473,   467 },
          {     98,   673,  -390 },
          {    -25,   603,  -404 },
        },
      },
    },
  },
  /* Green2: */
  [0] = {
    /* even columns: */
    {
      /* from frame A: */
      {
        /* from red (sum=-0.02): */
        {
          {     17,   -87,   -27 },
          {   -463,   497,   -26 },
          {    189,  -221,    -3 },
        },
        /* from green (sum=0.05): */
        {
          {  -2549,  2702,   -73 },
          {   -277,   537,    40 },
          {   3950, -3976,    47 },
        },
        /* from blue (sum=-0.08): */
        {
          {   2540, -2501,  -130 },
          {    881,  -987,  -292 },
          {  -4222,  4176,  -153 },
        },
      },
      /* from frame B: */
      {
        /* from red (sum=0.11): */
        {
          {    532,  -295,   -48 },
          {   3374, -2774,    35 },
          { -10387, 10458,    16 },
        },
        /* from green (sum=0.80): */
        {
          {   -515,   645,   -56 },
          {  -4905, 11107,   190 },
          {  10386,-10213,  -100 },
        },
        /* from blue (sum=0.14): */
        {
          {   -178,    95,   -34 },
          {   2130,  -686,   -29 },
          {   -161,   121,   -88 },
        },
      },
    },
    /* odd columns: */
    {
      /* from frame A: */
      {
        /* from red (sum=-0.18): */
        {
          {     -8,  -360,   355 },
          {   -113,   361, -1594 },
          {    138,    51,  -302 },
        },
        /* from green (sum=0.09): */
        {
          {     85,  8568, -8264 },
          {    128, -9194,  9167 },
          {     49, -1701,  1878 },
        },
        /* from blue (sum=0.04): */
        {
          {     32, -7766,  7688 },
          {     79,  9079, -8702 },
          {    -74,  1793, -1796 },
        },
      },
      /* from frame B: */
      {
        /* from red (sum=0.27): */
        {
          {   -243, 14363,-14345 },
          {    -68,   685,  2031 },
          {   -133, -2819,  2781 },
        },
        /* from green (sum=0.76): */
        {
          {    -80,-14095, 14148 },
          {    375,  8186, -2135 },
          {   -163,  3080, -3086 },
        },
        /* from blue (sum=0.02): */
        {
          {    -33,   157,   -43 },
          {      9,  -589,   554 },
          {    -19,    66,    52 },
        },
      },
    },
  },
  /* Blue: */
  [1] = {
    /* even columns: */
    {
      /* from frame A: */
      {
        /* from red (sum=0.00): */
        {
          {    567,  -577,   174 },
          {    114,   -44,  -384 },
          {    -72,   260,   -16 },
        },
        /* from green (sum=-0.03): */
        {
          {    617,  -393,   -14 },
          {   8087, -7671,  -710 },
          {    741,  -868,   -47 },
        },
        /* from blue (sum=0.81): */
        {
          {  -1344,  1352,   647 },
          {  -7923,  8321,  4918 },
          {   -735,   608,   793 },
        },
      },
      /* from frame B: */
      {
        /* from red (sum=-0.02): */
        {
          {  -3509,  3389,   -38 },
          {  -2787,  2812,   167 },
          {  14593,-14702,   -61 },
        },
        /* from green (sum=0.00): */
        {
          {   3399, -3717,  -259 },
          {   2445, -2093,   651 },
          { -14566, 14225,   -60 },
        },
        /* from blue (sum=0.23): */
        {
          {    174,  -115,    85 },
          {    566,   104,   707 },
          {    -48,   133,   312 },
        },
      },
    },
    /* odd columns: */
    {
      /* from frame A: */
      {
        /* from red (sum=0.06): */
        {
          {    -51,  -155,   410 },
          {     -8,  -281,   496 },
          {     91,   223,  -236 },
        },
        /* from green (sum=0.04): */
        {
          {    -29,  -688,  1069 },
          {    -60,  1345, -1225 },
          {     18, -1678,  1589 },
        },
        /* from blue (sum=0.13): */
        {
          {    -63,  1469, -1389 },
          {    197,  -240,   956 },
          {   -116,  1615, -1395 },
        },
      },
      /* from frame B: */
      {
        /* from red (sum=-0.08): */
        {
          {   -183,  6555, -6596 },
          {    -85, -4934,  4731 },
          {     20, -5176,  5053 },
        },
        /* from green (sum=-0.07): */
        {
          {    -11, -6824,  6288 },
          {     24,  3998, -3497 },
          {   -126,  5238, -5642 },
        },
        /* from blue (sum=0.92): */
        {
          {   -113,   850,   -39 },
          {    211,  6028,  -225 },
          {   -107,   710,   194 },
        },
      },
    },
  },
};

static void recover_bayer_channel(int dx, int dy)
{
    int ch = dx%2 + (dy%2) * 2;
    int w = width;
    int h = height;
    
    uint16_t* rgb[2] = {rgbA, rgbB};

    for (int y = 1; y < h-1; y++)
    {
        for (int x = 1; x < w-1; x++)
        {
            int sum = 0;

            /* separate filter for even/odd columns */
            int c = !(x % 2);

            /* we recover each channel from two frames: A and B */
            for (int k = 0; k < 2; k++)
            {
                /* for each channel, we have 3 predictors from each frame */
                for (int p = 0; p < 3; p++)
                {
                    #define filter filters[ch][c][k][p]
                    
                    sum +=
                        filter[0][0] * rgb[k][(x-1)*3+p + (y-1)*w*3] + filter[0][1] * rgb[k][x*3+p + (y-1)*w*3] + filter[0][2] * rgb[k][(x+1)*3+p + (y-1)*w*3] +
                        filter[1][0] * rgb[k][(x-1)*3+p + (y+0)*w*3] + filter[1][1] * rgb[k][x*3+p + (y+0)*w*3] + filter[1][2] * rgb[k][(x+1)*3+p + (y+0)*w*3] +
                        filter[2][0] * rgb[k][(x-1)*3+p + (y+1)*w*3] + filter[2][1] * rgb[k][x*3+p + (y+1)*w*3] + filter[2][2] * rgb[k][(x+1)*3+p + (y+1)*w*3] ;
                    
                    #undef filter
                }
            }

            raw[2*x+dx + (2*y+dy) * (2*width)] = COERCE(sum / 8192, 0, 65535);
        }
    }
}

/* recover raw data by filtering the two HDMI frames: rgbA and rgbB */
static void recover_raw_data()
{
    recover_bayer_channel(0,0);
    recover_bayer_channel(0,1);
    recover_bayer_channel(1,0);
    recover_bayer_channel(1,1);

    /* fill border pixels */
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            if (x == 0 || y == 0 || x == width-1 || y == height-1)
            {
                /* green1 from B, green2 from A, red/blue from B */
                raw[2*x   + (2*y  ) * (2*width)] = rgbB[3*x+1 + y*width*3];
                raw[2*x+1 + (2*y+1) * (2*width)] = rgbA[3*x+1 + y*width*3];
                raw[2*x   + (2*y+1) * (2*width)] = rgbB[3*x   + y*width*3];
                raw[2*x+1 + (2*y  ) * (2*width)] = rgbB[3*x+2 + y*width*3];
            }
        }
    }
}

int main(int argc, char** argv)
{
    if (argc == 1)
    {
        printf("HDMI RAW converter for Axiom BETA\n");
        printf("\n");
        printf("Usage:\n");
        printf("  ffmpeg -i input.mov -vf \"framestep=2\" frame%%05dA.ppm\n");
        printf("  ffmpeg -ss 00.016 -i input.mov -vf \"framestep=2\" frame%%05dB.ppm\n");
        printf("  %s frame*A.ppm\n", argv[0]);
        printf("  raw2dng frame*.pgm [options]\n");
        printf("\n");
        printf("Calibration files:\n");
        printf("  hdmi-darkframe-A.ppm, hdmi-darkframe-A.ppm:\n");
        printf("  averaged dark frames from the HDMI recorder (even/odd frames)\n");
        printf("\n");
        show_commandline_help(argv[0]);
        return 0;
    }

    /* parse all command-line options */
    for (int k = 1; k < argc; k++)
        if (argv[k][0] == '-')
            parse_commandline_option(argv[k]);
    show_active_options();

    printf("\n");
    
    /* all other arguments are input or output files */
    for (int k = 1; k < argc; k++)
    {
        if (argv[k][0] == '-')
            continue;

        char* out_filename;

        printf("\n%s\n", argv[k]);
        
        if (endswith(argv[k], "A.ppm"))
        {
            /* replace input file extension (including the A character) with .pgm */
            static char fo[256];
            int len = strlen(argv[k]) - 5;
            snprintf(fo, sizeof(fo), "%s", argv[k]);
            snprintf(fo+len, sizeof(fo)-len, ".pgm");
            out_filename = fo;
            
            read_ppm(argv[k], &rgbA);
            argv[k][len] = 'B';
            read_ppm(argv[k], &rgbB);
            raw = malloc(width * height * 4 * sizeof(raw[0]));
        }
        else if (endswith(argv[k], "B.ppm"))
        {
            printf("Ignored (please specify only A frames).\n");
            continue;
        }
        else if (endswith(argv[k], ".ppm"))
        {
            printf("Input files should end in A.ppm.\n");
            continue;
        }
        else
        {
            printf("Unknown file type.\n");
            continue;
        }
        
        printf("Recovering raw data...\n");
        recover_raw_data();
        
        printf("Convert to linear...\n");
        convert_to_linear(raw, 2*width, 2*height);
        
        printf("Output file : %s\n", out_filename);
        write_pgm(out_filename, raw, 2*width, 2*height);

        free(rgbA); rgbA = 0;
        free(rgbB); rgbB = 0;
        free(raw);  raw = 0;
    }
    
    printf("Done.\n\n");
    
    return 0;
}
