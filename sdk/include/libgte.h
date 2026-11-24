/* PsyQ-compatible <libgte.h> for the PC port: the geometry types and the library's routines, implemented in
 * software over the port's GTE (port/src/gte). */

#ifndef PORT_LIBGTE_H
#define PORT_LIBGTE_H

#include <sys/types.h>
#include "psxport.h"

#define ONE 4096

typedef struct {
    short m[3][3];
    long t[3];
} MATRIX;

typedef struct {
    long vx, vy;
    long vz, pad;
} VECTOR;

typedef struct {
    short vx, vy;
    short vz, pad;
} SVECTOR;

typedef struct {
    u_char r, g, b, cd;
} CVECTOR;

typedef struct {
    short vx, vy;
} DVECTOR;

typedef struct {
    SVECTOR v;
    VECTOR sxyz;
    DVECTOR sxy;
    CVECTOR rgb;
    short txuv, pad;
    long chx, chy;
} EVECTOR;

void InitGeom(void);
void SetGeomOffset(long ofx, long ofy);
void SetGeomScreen(long h);
long ReadGeomScreen(void);
void ReadGeomOffset(long *ofx, long *ofy);

MATRIX *RotMatrix(SVECTOR *r, MATRIX *m);
MATRIX *RotMatrixYXZ(SVECTOR *r, MATRIX *m);
MATRIX *RotMatrixZYX(SVECTOR *r, MATRIX *m);
MATRIX *RotMatrixX(long r, MATRIX *m);
MATRIX *RotMatrixY(long r, MATRIX *m);
MATRIX *RotMatrixZ(long r, MATRIX *m);
MATRIX *RotMatrix_gte(SVECTOR *r, MATRIX *m);
MATRIX *TransMatrix(MATRIX *m, VECTOR *v);
MATRIX *ScaleMatrix(MATRIX *m, VECTOR *v);
MATRIX *ScaleMatrixL(MATRIX *m, VECTOR *v);
MATRIX *TransposeMatrix(MATRIX *m0, MATRIX *m1);
MATRIX *MulMatrix(MATRIX *m0, MATRIX *m1);
MATRIX *MulMatrix0(MATRIX *m0, MATRIX *m1, MATRIX *m2);
MATRIX *MulMatrix2(MATRIX *m0, MATRIX *m1);
MATRIX *MulRotMatrix(MATRIX *m0);
MATRIX *MulRotMatrix0(MATRIX *m0, MATRIX *m1);
MATRIX *CompMatrix(MATRIX *m0, MATRIX *m1, MATRIX *m2);
MATRIX *CompMatrixLV(MATRIX *m0, MATRIX *m1, MATRIX *m2);
VECTOR *ApplyMatrix(MATRIX *m, SVECTOR *v0, VECTOR *v1);
SVECTOR *ApplyMatrixSV(MATRIX *m, SVECTOR *v0, SVECTOR *v1);
VECTOR *ApplyMatrixLV(MATRIX *m, VECTOR *v0, VECTOR *v1);
VECTOR *ApplyRotMatrix(SVECTOR *v0, VECTOR *v1);
VECTOR *ApplyRotMatrixLV(VECTOR *v0, VECTOR *v1);
void PushMatrix(void);
void PopMatrix(void);
void SetRotMatrix(MATRIX *m);
void SetTransMatrix(MATRIX *m);
void SetLightMatrix(MATRIX *m);
void SetColorMatrix(MATRIX *m);
void SetBackColor(long rbk, long gbk, long bbk);
void SetFarColor(long rfc, long gfc, long bfc);
void SetFogNear(long a, long h);
void SetFogNearFar(long a, long b, long h);
void SetDQA(int iDqa);
void SetDQB(int iDqb);
MATRIX *ReadRotMatrix(MATRIX *m);
MATRIX *ReadLightMatrix(MATRIX *m);
MATRIX *ReadColorMatrix(MATRIX *m);

long RotTransPers(SVECTOR *v0, long *sxy, long *p, long *flag);
long RotTransPers3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, long *sxy0, long *sxy1, long *sxy2, long *p,
                   long *flag);
long RotTransPers4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3, long *sxy0, long *sxy1, long *sxy2,
                   long *sxy3, long *p, long *flag);
void RotTrans(SVECTOR *v0, VECTOR *v1, long *flag);
long RotAverage3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, long *sxy0, long *sxy1, long *sxy2, long *p,
                 long *flag);
long RotAverage4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3, long *sxy0, long *sxy1, long *sxy2,
                 long *sxy3, long *p, long *flag);
long NormalClip(long sxy0, long sxy1, long sxy2);
long AverageZ3(long sz0, long sz1, long sz2);
long AverageZ4(long sz0, long sz1, long sz2, long sz3);
void LocalLight(SVECTOR *v0, VECTOR *v1);
void NormalColor(SVECTOR *v0, CVECTOR *v1);
void NormalColorCol(SVECTOR *v0, CVECTOR *v1, CVECTOR *v2);
void NormalColorDpq(SVECTOR *v0, CVECTOR *v1, long p, CVECTOR *v2);
void DpqColor(CVECTOR *v0, long p, CVECTOR *v1);
void ColorDpq(VECTOR *v0, CVECTOR *v1, long p, CVECTOR *v2);
void LoadAverage12(VECTOR *v0, VECTOR *v1, long p0, long p1, VECTOR *v2);
void LoadAverageShort12(SVECTOR *v0, SVECTOR *v1, long p0, long p1, SVECTOR *v2);
void LoadAverageCol(u_char *v0, u_char *v1, long p0, long p1, u_char *v2);

void LoadAverageByte(u_char *v0, u_char *v1, long p0, long p1, u_char *v2);
void ReadSZfifo3(long *sz0, long *sz1, long *sz2);
void ReadSZfifo4(long *szx, long *sz0, long *sz1, long *sz2);
void ReadSXSYfifo(long *sxy0, long *sxy1, long *sxy2);
void ReadRGBfifo(CVECTOR *v0, CVECTOR *v1, CVECTOR *v2);

void VectorNormal(VECTOR *v0, VECTOR *v1);
void VectorNormalS(VECTOR *v0, SVECTOR *v1);
void VectorNormalSS(SVECTOR *v0, SVECTOR *v1);
void OuterProduct0(VECTOR *v0, VECTOR *v1, VECTOR *v2);
void OuterProduct12(VECTOR *v0, VECTOR *v1, VECTOR *v2);
void Square0(VECTOR *v0, VECTOR *v1);
void Square12(VECTOR *v0, VECTOR *v1);
long SquareRoot0(long a);
long SquareRoot12(long a);
long Lzc(long data);

int rsin(int a);
int rcos(int a);
int csin(int a);
int ccos(int a);
int rcossin(int a);
long ratan2(long y, long x);
long catan(long a);

#endif
