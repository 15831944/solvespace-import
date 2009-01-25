
#ifndef __SURFACE_H
#define __SURFACE_H

// Utility functions, Bernstein polynomials of order 1-3 and their derivatives.
double Bernstein(int k, int deg, double t);
double BernsteinDerivative(int k, int deg, double t);

class SShell;

class hSSurface {
public:
    DWORD v;
};

class hSCurve {
public:
    DWORD v;
};

// Stuff for rational polynomial curves, of degree one to three. These are
// our inputs.
class SBezier {
public:
    int             tag;
    int             deg;
    Vector          ctrl[4];
    double          weight[4];

    Vector PointAt(double t);
    Vector Start(void);
    Vector Finish(void);
    void MakePwlInto(List<Vector> *l);
    void MakePwlInto(List<Vector> *l, Vector offset);
    void MakePwlWorker(List<Vector> *l, double ta, double tb, Vector offset);

    void Reverse(void);

    SBezier TransformedBy(Vector t, Quaternion q);

    static SBezier From(Vector p0, Vector p1, Vector p2, Vector p3);
    static SBezier From(Vector p0, Vector p1, Vector p2);
    static SBezier From(Vector p0, Vector p1);
};

class SBezierList {
public:
    List<SBezier>   l;

    void Clear(void);
};

class SBezierLoop {
public:
    List<SBezier>   l;

    inline void Clear(void) { l.Clear(); }
    void Reverse(void);
    void MakePwlInto(SContour *sc);

    static SBezierLoop FromCurves(SBezierList *spcl,
                                  bool *allClosed, SEdge *errorAt);
};

class SBezierLoopSet {
public:
    List<SBezierLoop> l;
    Vector normal;
    Vector point;

    static SBezierLoopSet From(SBezierList *spcl, SPolygon *poly,
                          bool *allClosed, SEdge *errorAt);

    void Clear(void);
};

// Stuff for the surface trim curves: piecewise linear
class SCurve {
public:
    hSCurve         h;

    bool            isExact;
    SBezier         exact;

    List<Vector>    pts;

    static SCurve SCurve::FromTransformationOf(SCurve *a,
                                               Vector t, Quaternion q);

    void Clear(void);
};

// A segment of a curve by which a surface is trimmed: indicates which curve,
// by its handle, and the starting and ending points of our segment of it.
// The vector out points out of the surface; it, the surface outer normal,
// and a tangent to the beginning of the curve are all orthogonal.
class STrimBy {
public:
    hSCurve     curve;
    bool        backwards;
    // If a trim runs backwards, then start and finish still correspond to
    // the actual start and finish, but they appear in reverse order in
    // the referenced curve.
    Vector      start;
    Vector      finish;

    static STrimBy STrimBy::EntireCurve(SShell *shell, hSCurve hsc, bool bkwds);
};

// A rational polynomial surface in Bezier form.
class SSurface {
public:
    hSSurface       h;

    int             color;
    DWORD           face;

    int             degm, degn;
    Vector          ctrl[4][4];
    double          weight[4][4];

    List<STrimBy>   trim;

    static SSurface FromExtrusionOf(SBezier *spc, Vector t0, Vector t1);
    static SSurface FromPlane(Vector pt, Vector n);
    static SSurface FromTransformationOf(SSurface *a, Vector t, Quaternion q, 
                                         bool includingTrims);

    void ClosestPointTo(Vector p, double *u, double *v);
    Vector PointAt(double u, double v);
    void TangentsAt(double u, double v, Vector *tu, Vector *tv);
    Vector NormalAt(double u, double v);

    void TriangulateInto(SShell *shell, SMesh *sm);
    void MakeEdgesInto(SShell *shell, SEdgeList *sel, bool asUv);

    void Clear(void);
};

class SShell {
public:
    IdList<SCurve,hSCurve>      curve;
    IdList<SSurface,hSSurface>  surface;

    void MakeFromExtrusionOf(SBezierLoopSet *sbls, Vector t0, Vector t1,
                             int color);
    void MakeFromUnionOf(SShell *a, SShell *b);
    void MakeFromDifferenceOf(SShell *a, SShell *b);
    void MakeFromCopyOf(SShell *a);
    void MakeFromTransformationOf(SShell *a, Vector trans, Quaternion q);

    void TriangulateInto(SMesh *sm);
    void MakeEdgesInto(SEdgeList *sel);
    void Clear(void);
};

#endif
