//-----------------------------------------------------------------------------
// OpenGL 2 based rendering interface.
//
// Copyright 2016 Aleksey Egorov
//-----------------------------------------------------------------------------
#include "solvespace.h"
#include "gl2shader.h"

namespace SolveSpace {

class TextureCache {
public:
    std::map<std::weak_ptr<const Pixmap>, GLuint,
             std::owner_less<std::weak_ptr<const Pixmap>>> items;

    bool Lookup(std::shared_ptr<const Pixmap> ptr, GLuint *result) {
        auto it = items.find(ptr);
        if(it == items.end()) {
            GLuint id;
            glGenTextures(1, &id);
            items[ptr] = id;
            *result = id;
            return false;
        }

        *result = it->second;
        return true;
    }

    void CleanupUnused() {
        for(auto it = items.begin(); it != items.end();) {
            if(it->first.expired()) {
                glDeleteTextures(1, &it->second);
                it = items.erase(it);
                continue;
            }
            it++;
        }
    }
};

// A canvas that uses the core OpenGL 2 profile, for desktop systems.
class OpenGl2Renderer : public ViewportCanvas {
public:
    struct SEdgeListItem {
        hStroke         h;
        SEdgeList       lines;

        void Clear() { lines.Clear(); }
    };

    struct SMeshListItem {
        hFill           h;
        SIndexedMesh    mesh;

        void Clear() { mesh.Clear(); }
    };

    struct SPointListItem {
        hStroke         h;
        SIndexedMesh    points;

        void Clear() { points.Clear(); }
    };

    IdList<SEdgeListItem,  hStroke> lines;
    IdList<SMeshListItem,  hFill>   meshes;
    IdList<SPointListItem, hStroke> points;

    TextureCache            pixmapCache;
    std::shared_ptr<Pixmap> masks[3];

    bool                initialized;
    StippleAtlas        atlas;
    MeshRenderer        meshRenderer;
    IndexedMeshRenderer imeshRenderer;
    EdgeRenderer        edgeRenderer;
    OutlineRenderer     outlineRenderer;

    Camera   camera;
    Lighting lighting;
    // Cached OpenGL state.
    struct {
        hStroke     hcs;
        Stroke     *stroke;
        hFill       hcf;
        Fill       *fill;
        std::weak_ptr<const Pixmap> texture;
    } current;

    OpenGl2Renderer() :
        lines(), meshes(), points(), pixmapCache(), masks(),
        initialized(), atlas(), meshRenderer(), imeshRenderer(),
        edgeRenderer(), outlineRenderer(), camera(), lighting(),
        current() {}

    void Init();

    const Camera &GetCamera() const override { return camera; }

    void DrawLine(const Vector &a, const Vector &b, hStroke hcs) override;
    void DrawEdges(const SEdgeList &el, hStroke hcs) override;
    void DrawEdgesInternal(const SEdgeList &el, hStroke hcs);
    bool DrawBeziers(const SBezierList &bl, hStroke hcs) override { return false; }
    void DrawOutlines(const SOutlineList &ol, hStroke hcs, DrawOutlinesAs mode) override;
    void DrawVectorText(const std::string &text, double height,
                        const Vector &o, const Vector &u, const Vector &v,
                        hStroke hcs) override;

    void DrawQuad(const Vector &a, const Vector &b, const Vector &c, const Vector &d,
                  hFill hcf) override;
    void DrawPoint(const Vector &o, hStroke hs) override;
    void DrawPolygon(const SPolygon &p, hFill hcf) override;
    void DrawMesh(const SMesh &m, hFill hcfFront, hFill hcfBack) override;
    void DrawFaces(const SMesh &m, const std::vector<uint32_t> &faces, hFill hcf) override;
    void DrawPixmap(std::shared_ptr<const Pixmap> pm,
                    const Vector &o, const Vector &u, const Vector &v,
                    const Point2d &ta, const Point2d &tb, hFill hcf) override;
    void InvalidatePixmap(std::shared_ptr<const Pixmap> pm) override;

    std::shared_ptr<BatchCanvas> CreateBatch() override;

    Stroke *SelectStroke(hStroke hcs);
    Fill *SelectFill(hFill hcf);
    void SelectMask(FillPattern pattern);
    void SelectTexture(std::shared_ptr<const Pixmap> pm);
    void DoFatLineEndcap(const Vector &p, const Vector &u, const Vector &v);
    void DoFatLine(const Vector &a, const Vector &b, double width);
    void DoLine(const Vector &a, const Vector &b, hStroke hcs);
    void DoPoint(Vector p, hStroke hs);
    void DoStippledLine(const Vector &a, const Vector &b, hStroke hcs);

    void UpdateProjection(bool flip = FLIP_FRAMEBUFFER);
    void SetCamera(const Camera &c, bool flip) override;
    void SetLighting(const Lighting &l) override;

    void BeginFrame() override;
    void EndFrame() override;
    std::shared_ptr<Pixmap> ReadFrame() override;

    void GetIdent(const char **vendor, const char **renderer, const char **version) override;
};

//-----------------------------------------------------------------------------
// Thin wrappers around OpenGL functions to fix bugs, adapt them to our
// data structures, etc.
//-----------------------------------------------------------------------------

static void ssglDepthRange(Canvas::Layer layer, int zIndex) {
    switch(layer) {
        case Canvas::Layer::NORMAL:
        case Canvas::Layer::FRONT:
        case Canvas::Layer::BACK:
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_TRUE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            break;

        case Canvas::Layer::DEPTH_ONLY:
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_TRUE);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            break;

        case Canvas::Layer::OCCLUDED:
            glDepthFunc(GL_GREATER);
            glDepthMask(GL_FALSE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            break;
    }

    switch(layer) {
        case Canvas::Layer::FRONT:
            glDepthRange(0.0, 0.0);
            break;

        case Canvas::Layer::BACK:
            glDepthRange(1.0, 1.0);
            break;

        case Canvas::Layer::NORMAL:
        case Canvas::Layer::DEPTH_ONLY:
        case Canvas::Layer::OCCLUDED:
            // The size of this step depends on the resolution of the Z buffer; for
            // a 16-bit buffer, this should be fine.
            double offset = 1.0 / (65535 * 0.8) * zIndex;
            glDepthRange(0.1 - offset, 1.0 - offset);
            break;
    }
}

//-----------------------------------------------------------------------------
// A simple OpenGL state tracker to group consecutive draw calls.
//-----------------------------------------------------------------------------

Canvas::Stroke *OpenGl2Renderer::SelectStroke(hStroke hcs) {
    if(current.hcs.v == hcs.v) return current.stroke;

    Stroke *stroke = strokes.FindById(hcs);
    ssglDepthRange(stroke->layer, stroke->zIndex);

    current.hcs    = hcs;
    current.stroke = stroke;
    current.hcf    = {};
    current.fill   = NULL;
    current.texture.reset();
    return stroke;
}

void OpenGl2Renderer::SelectMask(FillPattern pattern) {
    if(!masks[0]) {
        masks[0] = Pixmap::Create(Pixmap::Format::A, 32, 32);
        masks[1] = Pixmap::Create(Pixmap::Format::A, 32, 32);
        masks[2] = Pixmap::Create(Pixmap::Format::A, 32, 32);

        for(int x = 0; x < 32; x++) {
            for(int y = 0; y < 32; y++) {
                masks[0]->data[y * 32 + x] = ((x / 2) % 2 == 0 && (y / 2) % 2 == 0) ? 0xFF : 0x00;
                masks[1]->data[y * 32 + x] = ((x / 2) % 2 == 1 && (y / 2) % 2 == 1) ? 0xFF : 0x00;
                masks[2]->data[y * 32 + x] = 0xFF;
            }
        }
    }

    switch(pattern) {
        case Canvas::FillPattern::SOLID:
            SelectTexture(masks[2]);
            break;

        case Canvas::FillPattern::CHECKERED_A:
            SelectTexture(masks[0]);
            break;

        case Canvas::FillPattern::CHECKERED_B:
            SelectTexture(masks[1]);
            break;

        default: ssassert(false, "Unexpected fill pattern");
    }
}

Canvas::Fill *OpenGl2Renderer::SelectFill(hFill hcf) {
    if(current.hcf.v == hcf.v) return current.fill;

    Fill *fill = fills.FindById(hcf);
    ssglDepthRange(fill->layer, fill->zIndex);

    current.hcs    = {};
    current.stroke = NULL;
    current.hcf    = hcf;
    current.fill   = fill;
    if(fill->pattern != FillPattern::SOLID) {
        SelectMask(fill->pattern);
    } else if(fill->texture) {
        SelectTexture(fill->texture);
    } else {
        SelectMask(FillPattern::SOLID);
    }
    return fill;
}

void OpenGl2Renderer::InvalidatePixmap(std::shared_ptr<const Pixmap> pm) {
    GLuint id;
    pixmapCache.Lookup(pm, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_REPEAT);

    GLenum format;
    switch(pm->format) {
        case Pixmap::Format::RGBA: format = GL_RGBA;  break;
        case Pixmap::Format::RGB:  format = GL_RGB;   break;
        case Pixmap::Format::A:    format = GL_ALPHA; break;
        case Pixmap::Format::BGRA:
        case Pixmap::Format::BGR:
            ssassert(false, "Unexpected pixmap format");
    }
    glTexImage2D(GL_TEXTURE_2D, 0, format, pm->width, pm->height, 0,
                 format, GL_UNSIGNED_BYTE, &pm->data[0]);
}

void OpenGl2Renderer::SelectTexture(std::shared_ptr<const Pixmap> pm) {
    if(current.texture.lock() == pm) return;

    GLuint id;
    if(!pixmapCache.Lookup(pm, &id)) {
        InvalidatePixmap(pm);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, id);
    current.texture = pm;
}

void OpenGl2Renderer::DoLine(const Vector &a, const Vector &b, hStroke hcs) {
    SEdgeListItem *eli = lines.FindByIdNoOops(hcs);
    if(eli == NULL) {
        SEdgeListItem item = {};
        item.h = hcs;
        lines.Add(&item);
        eli = lines.FindByIdNoOops(hcs);
    }

    eli->lines.AddEdge(a, b);
}

void OpenGl2Renderer::DoPoint(Vector p, hStroke hs) {
    SPointListItem *pli = points.FindByIdNoOops(hs);
    if(pli == NULL) {
        SPointListItem item = {};
        item.h = hs;
        points.Add(&item);
        pli = points.FindByIdNoOops(hs);
    }

    pli->points.AddPoint(p);
}

void OpenGl2Renderer::DoStippledLine(const Vector &a, const Vector &b, hStroke hcs) {
    Stroke *stroke = strokes.FindById(hcs);
    if(stroke->stipplePattern != StipplePattern::FREEHAND &&
       stroke->stipplePattern != StipplePattern::ZIGZAG)
    {
        DoLine(a, b, hcs);
        return;
    }

    const char *patternSeq;
    Stroke s = *stroke;
    s.stipplePattern = StipplePattern::CONTINUOUS;
    hcs = GetStroke(s);
    switch(stroke->stipplePattern) {
        case StipplePattern::CONTINUOUS:    DoLine(a, b, hcs);  return;
        case StipplePattern::SHORT_DASH:    patternSeq = "-  "; break;
        case StipplePattern::DASH:          patternSeq = "- ";  break;
        case StipplePattern::LONG_DASH:     patternSeq = "_ ";  break;
        case StipplePattern::DASH_DOT:      patternSeq = "-.";  break;
        case StipplePattern::DASH_DOT_DOT:  patternSeq = "-.."; break;
        case StipplePattern::DOT:           patternSeq = ".";   break;
        case StipplePattern::FREEHAND:      patternSeq = "~";   break;
        case StipplePattern::ZIGZAG:        patternSeq = "~__"; break;
    }

    Vector dir = b.Minus(a);
    double len = dir.Magnitude();
    dir = dir.WithMagnitude(1.0);

    const char *si = patternSeq;
    double end = len;
    double ss = stroke->stippleScale / 2.0;
    do {
        double start = end;
        switch(*si) {
            case ' ':
                end -= 1.0 * ss;
                break;

            case '-':
                start = max(start - 0.5 * ss, 0.0);
                end = max(start - 2.0 * ss, 0.0);
                if(start == end) break;
                DoLine(a.Plus(dir.ScaledBy(start)), a.Plus(dir.ScaledBy(end)), hcs);
                end = max(end - 0.5 * ss, 0.0);
                break;

            case '_':
                end = max(end - 4.0 * ss, 0.0);
                DoLine(a.Plus(dir.ScaledBy(start)), a.Plus(dir.ScaledBy(end)), hcs);
                break;

            case '.':
                end = max(end - 0.5 * ss, 0.0);
                if(end == 0.0) break;
                DoPoint(a.Plus(dir.ScaledBy(end)), hcs);
                end = max(end - 0.5 * ss, 0.0);
                break;

            case '~': {
                Vector ab  = b.Minus(a);
                Vector gn = (camera.projRight).Cross(camera.projUp);
                Vector abn = (ab.Cross(gn)).WithMagnitude(1);
                abn = abn.Minus(gn.ScaledBy(gn.Dot(abn)));
                double pws = 2.0 * stroke->width / camera.scale;

                end = max(end - 0.5 * ss, 0.0);
                Vector aa = a.Plus(dir.ScaledBy(start));
                Vector bb = a.Plus(dir.ScaledBy(end))
                             .Plus(abn.ScaledBy(pws * (start - end) / (0.5 * ss)));
                DoLine(aa, bb, hcs);
                if(end == 0.0) break;

                start = end;
                end = max(end - 1.0 * ss, 0.0);
                aa = a.Plus(dir.ScaledBy(end))
                      .Plus(abn.ScaledBy(pws))
                      .Minus(abn.ScaledBy(2.0 * pws * (start - end) / ss));
                DoLine(bb, aa, hcs);
                if(end == 0.0) break;

                start = end;
                end = max(end - 0.5 * ss, 0.0);
                bb = a.Plus(dir.ScaledBy(end))
                      .Minus(abn.ScaledBy(pws))
                      .Plus(abn.ScaledBy(pws * (start - end) / (0.5 * ss)));
                DoLine(aa, bb, hcs);
                break;
            }

            default: ssassert(false, "Unexpected stipple pattern element");
        }
        if(*(++si) == 0) si = patternSeq;
    } while(end > 0.0);
}

//-----------------------------------------------------------------------------
// A canvas implemented using OpenGL 2 vertex buffer objects.
//-----------------------------------------------------------------------------

void OpenGl2Renderer::Init() {
    atlas.Init();
    edgeRenderer.Init(&atlas);
    outlineRenderer.Init(&atlas);
    meshRenderer.Init();
    imeshRenderer.Init();
}

void OpenGl2Renderer::DrawLine(const Vector &a, const Vector &b, hStroke hcs) {
    DoStippledLine(a, b, hcs);
}

void OpenGl2Renderer::DrawEdges(const SEdgeList &el, hStroke hcs) {
    for(const SEdge &e : el.l) {
        DoStippledLine(e.a, e.b, hcs);
    }
}

void OpenGl2Renderer::DrawEdgesInternal(const SEdgeList &el, hStroke hcs) {
    if(el.l.n == 0) return;

    Stroke *stroke = SelectStroke(hcs);
    if(stroke->stipplePattern == StipplePattern::ZIGZAG ||
       stroke->stipplePattern == StipplePattern::FREEHAND)
    {
        for(const SEdge *e = el.l.First(); e; e = el.l.NextAfter(e)) {
            DoStippledLine(e->a, e->b, hcs);
        }
        return;
    }

    edgeRenderer.SetStroke(*stroke, 1.0 / camera.scale);
    edgeRenderer.Draw(el);
}

void OpenGl2Renderer::DrawOutlines(const SOutlineList &ol, hStroke hcs, DrawOutlinesAs mode) {
    if(ol.l.n == 0) return;

    Stroke *stroke = SelectStroke(hcs);
    ssassert(stroke->stipplePattern != StipplePattern::ZIGZAG &&
             stroke->stipplePattern != StipplePattern::FREEHAND,
             "ZIGZAG and FREEHAND not supported for outlines");

    outlineRenderer.SetStroke(*stroke, 1.0 / camera.scale);
    outlineRenderer.Draw(ol, mode);
}

void OpenGl2Renderer::DrawVectorText(const std::string &text, double height,
                                     const Vector &o, const Vector &u, const Vector &v,
                                     hStroke hcs) {
    SEdgeList el = {};
    auto traceEdge = [&](Vector a, Vector b) { el.AddEdge(a, b); };
    VectorFont::Builtin()->Trace(height, o, u, v, text, traceEdge, camera);
    DrawEdgesInternal(el, hcs);
    el.Clear();
}

void OpenGl2Renderer::DrawQuad(const Vector &a, const Vector &b, const Vector &c, const Vector &d,
                               hFill hcf) {
    SMeshListItem *li = meshes.FindByIdNoOops(hcf);
    if(li == NULL) {
        SMeshListItem item = {};
        item.h = hcf;
        meshes.Add(&item);
        li = meshes.FindByIdNoOops(hcf);
    }
    li->mesh.AddQuad(a, b, c, d);
}

void OpenGl2Renderer::DrawPoint(const Vector &o, hStroke hs) {
    DoPoint(o, hs);
}

void OpenGl2Renderer::DrawPolygon(const SPolygon &p, hFill hcf) {
    Fill *fill = SelectFill(hcf);

    SMesh m = {};
    p.TriangulateInto(&m);
    meshRenderer.UseFilled(*fill);
    meshRenderer.Draw(m);
    m.Clear();
}

void OpenGl2Renderer::DrawMesh(const SMesh &m, hFill hcfFront, hFill hcfBack) {
    ssassert(false, "Not implemented");
}

void OpenGl2Renderer::DrawFaces(const SMesh &m, const std::vector<uint32_t> &faces, hFill hcf) {
    if(faces.empty()) return;

    Fill *fill = SelectFill(hcf);

    SMesh facesMesh = {};
    for(uint32_t f : faces) {
        for(const STriangle &t : m.l) {
            if(f != t.meta.face) continue;
            facesMesh.l.Add(&t);
        }
    }

    meshRenderer.UseFilled(*fill);
    meshRenderer.Draw(facesMesh);
    facesMesh.Clear();
}

void OpenGl2Renderer::DrawPixmap(std::shared_ptr<const Pixmap> pm,
                                 const Vector &o, const Vector &u, const Vector &v,
                                 const Point2d &ta, const Point2d &tb, hFill hcf) {
    Fill fill = *fills.FindById(hcf);
    fill.texture = pm;
    hcf = GetFill(fill);

    SMeshListItem *mli = meshes.FindByIdNoOops(hcf);
    if(mli == NULL) {
        SMeshListItem item = {};
        item.h = hcf;
        meshes.Add(&item);
        mli = meshes.FindByIdNoOops(hcf);
    }

    mli->mesh.AddPixmap(o, u, v, ta, tb);
}

void OpenGl2Renderer::UpdateProjection(bool flip) {
    glViewport(0, 0, camera.width, camera.height);

    double mat1[16];
    double mat2[16];

    double sx = camera.scale * 2.0 / camera.width;
    double sy = camera.scale * 2.0 / camera.height;
    double sz = camera.scale * 1.0 / 30000;

    MakeMatrix(mat1,
       sx,   0,   0,   0,
        0,  sy,   0,   0,
        0,   0,  sz,   0,
        0,   0,   0,   1
    );

    // Last thing before display is to apply the perspective
    double clp = camera.tangent * camera.scale;
    double fy = flip ? -1.0 : 1.0;
    MakeMatrix(mat2,
        1,   0,   0,   0,
        0,  fy,   0,   0,
        0,   0,   1,   0,
        0,   0,   clp, 1
    );

    // If we flip the framebuffer, then we also flip the handedness
    // of the coordinate system, and so the face winding order.
    glFrontFace(flip ? GL_CW : GL_CCW);

    double projection[16];
    MultMatrix(mat1, mat2, projection);

    // Before that, we apply the rotation
    Vector u = camera.projRight,
           v = camera.projUp,
           n = camera.projUp.Cross(camera.projRight);
    MakeMatrix(mat1,
        u.x, u.y, u.z,   0,
        v.x, v.y, v.z,   0,
        n.x, n.y, n.z,   0,
          0,   0,   0,   1
    );

    // And before that, the translation
    Vector o = camera.offset;
    MakeMatrix(mat2,
        1, 0, 0, o.x,
        0, 1, 0, o.y,
        0, 0, 1, o.z,
        0, 0, 0,   1
    );

    double modelview[16];
    MultMatrix(mat1, mat2, modelview);

    imeshRenderer.SetProjection(projection);
    imeshRenderer.SetModelview(modelview);
    meshRenderer.SetProjection(projection);
    meshRenderer.SetModelview(modelview);
    edgeRenderer.SetProjection(projection);
    edgeRenderer.SetModelview(modelview);
    outlineRenderer.SetProjection(projection);
    outlineRenderer.SetModelview(modelview);

    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void OpenGl2Renderer::BeginFrame() {
    if(!initialized) {
        Init();
        initialized = true;
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);

    glDepthFunc(GL_LEQUAL);
    glEnable(GL_DEPTH_TEST);

    RgbaColor backgroundColor = lighting.backgroundColor;
    glClearColor(backgroundColor.redF(),  backgroundColor.greenF(),
                 backgroundColor.blueF(), backgroundColor.alphaF());
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glPolygonOffset(2.0, 1.0);
}

void OpenGl2Renderer::EndFrame() {
    for(SMeshListItem &li : meshes) {
        Fill *fill = SelectFill(li.h);

        imeshRenderer.UseFilled(*fill);
        imeshRenderer.Draw(li.mesh);
        li.mesh.Clear();
    }
    meshes.Clear();

    for(SEdgeListItem &eli : lines) {
        DrawEdgesInternal(eli.lines, eli.h);
        eli.lines.Clear();
    }
    lines.Clear();

    for(SPointListItem &li : points) {
        Stroke *stroke = SelectStroke(li.h);

        imeshRenderer.UsePoint(*stroke, 1.0 / camera.scale);
        imeshRenderer.Draw(li.points);
        li.points.Clear();
    }
    points.Clear();

    glFinish();

    GLenum error = glGetError();
    if(error != GL_NO_ERROR) {
        dbp("glGetError() == 0x%X", error);
    }
}

std::shared_ptr<Pixmap> OpenGl2Renderer::ReadFrame() {
    std::shared_ptr<Pixmap> pixmap =
        Pixmap::Create(Pixmap::Format::RGB, (size_t)camera.width, (size_t)camera.height);
    glReadPixels(0, 0, camera.width, camera.height, GL_RGB, GL_UNSIGNED_BYTE, &pixmap->data[0]);
    return pixmap;
}

void OpenGl2Renderer::GetIdent(const char **vendor, const char **renderer, const char **version) {
    *vendor   = (const char *)glGetString(GL_VENDOR);
    *renderer = (const char *)glGetString(GL_RENDERER);
    *version  = (const char *)glGetString(GL_VERSION);
}

void OpenGl2Renderer::SetCamera(const Camera &c, bool flip) {
    camera = c;
    UpdateProjection(flip);
}

void OpenGl2Renderer::SetLighting(const Lighting &l) {
    lighting = l;
}

//-----------------------------------------------------------------------------
// A batch canvas implemented using OpenGL 2 vertex buffer objects.
//-----------------------------------------------------------------------------

class DrawCall {
public:
    virtual Canvas::Layer GetLayer() const = 0;
    virtual int GetZIndex() const = 0;

    virtual void Draw(OpenGl2Renderer *renderer) = 0;
    virtual void Remove(OpenGl2Renderer *renderer) = 0;
};

class EdgeDrawCall : public DrawCall {
public:
    // Key
    Canvas::Stroke             *stroke;
    // Data
    EdgeRenderer::Handle        handle;

    virtual Canvas::Layer GetLayer() const override { return stroke->layer; };
    virtual int GetZIndex() const override { return stroke->zIndex; };

    static std::shared_ptr<DrawCall> Create(OpenGl2Renderer *renderer, const SEdgeList &el,
                                            Canvas::Stroke *stroke) {
        EdgeDrawCall *dc = new EdgeDrawCall();
        dc->stroke = stroke;
        dc->handle = renderer->edgeRenderer.Add(el);
        return std::shared_ptr<DrawCall>(dc);
    }

    void Draw(OpenGl2Renderer *renderer) override {
        ssglDepthRange(stroke->layer, stroke->zIndex);
        renderer->edgeRenderer.SetStroke(*stroke, 1.0 / renderer->camera.scale);
        renderer->edgeRenderer.Draw(handle);
    }

    void Remove(OpenGl2Renderer *renderer) override {
        renderer->edgeRenderer.Remove(handle);
    }
};

class OutlineDrawCall : public DrawCall {
public:
    // Key
    Canvas::Stroke             *stroke;
    // Data
    OutlineRenderer::Handle     handle;
    Canvas::DrawOutlinesAs      drawAs;

    virtual Canvas::Layer GetLayer() const override { return stroke->layer; };
    virtual int GetZIndex() const override { return stroke->zIndex; };

    static std::shared_ptr<DrawCall> Create(OpenGl2Renderer *renderer, const SOutlineList &ol,
                                            Canvas::Stroke *stroke,
                                            Canvas::DrawOutlinesAs drawAs) {
        OutlineDrawCall *dc = new OutlineDrawCall();
        dc->stroke = stroke;
        dc->handle = renderer->outlineRenderer.Add(ol);
        dc->drawAs = drawAs;
        return std::shared_ptr<DrawCall>(dc);
    }

    void Draw(OpenGl2Renderer *renderer) override {
        ssglDepthRange(stroke->layer, stroke->zIndex);
        renderer->outlineRenderer.SetStroke(*stroke, 1.0 / renderer->camera.scale);
        renderer->outlineRenderer.Draw(handle, drawAs);
    }

    void Remove(OpenGl2Renderer *renderer) override {
        renderer->outlineRenderer.Remove(handle);
    }
};

class PointDrawCall : public DrawCall {
public:
    // Key
    Canvas::Stroke              *stroke;
    // Data
    IndexedMeshRenderer::Handle  handle;

    virtual Canvas::Layer GetLayer() const override { return stroke->layer; };
    virtual int GetZIndex() const override { return stroke->zIndex; };

    static std::shared_ptr<DrawCall> Create(OpenGl2Renderer *renderer, const SIndexedMesh &im,
                                            Canvas::Stroke *stroke) {
        PointDrawCall *dc = new PointDrawCall();
        dc->stroke = stroke;
        dc->handle = renderer->imeshRenderer.Add(im);
        return std::shared_ptr<DrawCall>(dc);
    }

    void Draw(OpenGl2Renderer *renderer) override {
        ssglDepthRange(stroke->layer, stroke->zIndex);
        renderer->imeshRenderer.UsePoint(*stroke, 1.0 / renderer->camera.scale);
        renderer->imeshRenderer.Draw(handle);
    }

    void Remove(OpenGl2Renderer *renderer) override {
        renderer->imeshRenderer.Remove(handle);
    }
};

class MeshDrawCall : public DrawCall {
public:
    // Key
    Canvas::Fill           *fillFront;
    // Data
    MeshRenderer::Handle    handle;
    Canvas::Fill           *fillBack;
    bool                    isShaded;

    virtual Canvas::Layer GetLayer() const override { return fillFront->layer; };
    virtual int GetZIndex() const override { return fillFront->zIndex; };

    static std::shared_ptr<DrawCall> Create(OpenGl2Renderer *renderer, const SMesh &m,
                                            Canvas::Fill *fillFront, Canvas::Fill *fillBack = NULL,
                                            bool isShaded = false) {
        MeshDrawCall *dc = new MeshDrawCall();
        dc->fillFront       = fillFront;
        dc->handle          = renderer->meshRenderer.Add(m);
        dc->fillBack        = fillBack;
        dc->isShaded        = isShaded;
        return std::shared_ptr<DrawCall>(dc);
    }

    void DrawFace(OpenGl2Renderer *renderer, GLenum cullFace, Canvas::Fill *fill) {
        glCullFace(cullFace);
        ssglDepthRange(fill->layer, fill->zIndex);
        if(fill->pattern != Canvas::FillPattern::SOLID) {
            renderer->SelectMask(fill->pattern);
        } else if(fill->texture) {
            renderer->SelectTexture(fill->texture);
        } else {
            renderer->SelectMask(Canvas::FillPattern::SOLID);
        }
        if(isShaded) {
            renderer->meshRenderer.UseShaded(renderer->lighting);
        } else {
            renderer->meshRenderer.UseFilled(*fill);
        }
        renderer->meshRenderer.Draw(handle, /*useColors=*/fill->color.IsEmpty(), fill->color);
    }

    void Draw(OpenGl2Renderer *renderer) override {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glEnable(GL_CULL_FACE);

        if(fillBack)
            DrawFace(renderer, GL_FRONT, fillBack);
        DrawFace(renderer, GL_BACK, fillFront);

        glDisable(GL_POLYGON_OFFSET_FILL);
        glDisable(GL_CULL_FACE);
    }

    void Remove(OpenGl2Renderer *renderer) override {
        renderer->meshRenderer.Remove(handle);
    }
};

struct CompareDrawCall {
    bool operator()(const std::shared_ptr<DrawCall> &a, const std::shared_ptr<DrawCall> &b) {
        const Canvas::Layer stackup[] = {
            Canvas::Layer::BACK,
            Canvas::Layer::NORMAL,
            Canvas::Layer::DEPTH_ONLY,
            Canvas::Layer::OCCLUDED,
            Canvas::Layer::FRONT
        };

        int aLayerIndex =
            std::find(std::begin(stackup), std::end(stackup), a->GetLayer()) - std::begin(stackup);
        int bLayerIndex =
            std::find(std::begin(stackup), std::end(stackup), b->GetLayer()) - std::begin(stackup);
        if(aLayerIndex == bLayerIndex) {
            return a->GetZIndex() < b->GetZIndex();
        } else {
            return aLayerIndex < bLayerIndex;
        }
    }
};

class OpenGl2RendererBatch : public BatchCanvas {
public:
    struct EdgeBuffer {
        hStroke         h;
        SEdgeList       edges;

        void Clear() {
            edges.Clear();
        }
    };

    struct PointBuffer {
        hStroke         h;
        SIndexedMesh    points;

        void Clear() {
            points.Clear();
        }
    };

    OpenGl2Renderer *renderer;

    IdList<EdgeBuffer,  hStroke> edgeBuffer;
    IdList<PointBuffer, hStroke> pointBuffer;

    std::multiset<std::shared_ptr<DrawCall>, CompareDrawCall> drawCalls;

    OpenGl2RendererBatch() : renderer(), edgeBuffer(), pointBuffer() {}

    void DrawLine(const Vector &a, const Vector &b, hStroke hcs) override {
        EdgeBuffer *eb = edgeBuffer.FindByIdNoOops(hcs);
        if(!eb) {
            EdgeBuffer neb = {};
            neb.h = hcs;
            edgeBuffer.Add(&neb);
            eb = edgeBuffer.FindById(hcs);
        }

        eb->edges.AddEdge(a, b);
    }

    void DrawEdges(const SEdgeList &el, hStroke hcs) override {
        EdgeBuffer *eb = edgeBuffer.FindByIdNoOops(hcs);
        if(!eb) {
            EdgeBuffer neb = {};
            neb.h = hcs;
            edgeBuffer.Add(&neb);
            eb = edgeBuffer.FindById(hcs);
        }

        for(const SEdge &e : el.l) {
            eb->edges.AddEdge(e.a, e.b);
        }
    }

    bool DrawBeziers(const SBezierList &bl, hStroke hcs) override {
        return false;
    }

    void DrawOutlines(const SOutlineList &ol, hStroke hcs, DrawOutlinesAs drawAs) override {
        drawCalls.emplace(OutlineDrawCall::Create(renderer, ol, strokes.FindById(hcs), drawAs));
    }

    void DrawVectorText(const std::string &text, double height,
                        const Vector &o, const Vector &u, const Vector &v,
                        hStroke hcs) override {
        ssassert(false, "Not implemented");
    }

    void DrawQuad(const Vector &a, const Vector &b, const Vector &c, const Vector &d,
                  hFill hcf) override {
        ssassert(false, "Not implemented");
    }

    void DrawPoint(const Vector &o, hStroke hcs) override {
        PointBuffer *pb = pointBuffer.FindByIdNoOops(hcs);
        if(!pb) {
            PointBuffer npb = {};
            npb.h = hcs;
            pointBuffer.Add(&npb);
            pb = pointBuffer.FindById(hcs);
        }

        pb->points.AddPoint(o);
    }

    void DrawPolygon(const SPolygon &p, hFill hcf) override {
        SMesh m = {};
        p.TriangulateInto(&m);
        drawCalls.emplace(MeshDrawCall::Create(renderer, m, fills.FindById(hcf),
                                               fills.FindById(hcf)));
        m.Clear();
    }

    void DrawMesh(const SMesh &m, hFill hcfFront, hFill hcfBack = {}) override {
        drawCalls.emplace(MeshDrawCall::Create(renderer, m, fills.FindById(hcfFront),
                                               fills.FindByIdNoOops(hcfBack),
                                               /*lighting=*/true));
    }

    void DrawFaces(const SMesh &m, const std::vector<uint32_t> &faces, hFill hcf) override {
        ssassert(false, "Not implemented");
    }

    void DrawPixmap(std::shared_ptr<const Pixmap> pm,
                    const Vector &o, const Vector &u, const Vector &v,
                    const Point2d &ta, const Point2d &tb, hFill hcf) override {
        ssassert(false, "Not implemented");
    }

    void InvalidatePixmap(std::shared_ptr<const Pixmap> pm) override {
        ssassert(false, "Not implemented");
    }

    void Finalize() override {
        for(const EdgeBuffer &eb : edgeBuffer) {
            drawCalls.emplace(EdgeDrawCall::Create(renderer, eb.edges, strokes.FindById(eb.h)));
        }
        edgeBuffer.Clear();

        for(const PointBuffer &pb : pointBuffer) {
            drawCalls.emplace(PointDrawCall::Create(renderer, pb.points, strokes.FindById(pb.h)));
        }
        pointBuffer.Clear();
    }

    void Draw() override {
        renderer->current = {};

        for(std::shared_ptr<DrawCall> dc : drawCalls) {
            dc->Draw(renderer);
        }
    }

    void Clear() override {
        for(std::shared_ptr<DrawCall> dc : drawCalls) {
            dc->Remove(renderer);
        }
        drawCalls.clear();
    }
};

//-----------------------------------------------------------------------------
// Factory functions.
//-----------------------------------------------------------------------------

std::shared_ptr<BatchCanvas> OpenGl2Renderer::CreateBatch() {
    OpenGl2RendererBatch *batch = new OpenGl2RendererBatch();
    batch->renderer = this;
    return std::shared_ptr<BatchCanvas>(batch);
}

std::shared_ptr<ViewportCanvas> CreateRenderer() {
    return std::shared_ptr<ViewportCanvas>(new OpenGl2Renderer());
}

}
