// Copyright 2021-present Contributors to the OpenImageIO project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/OpenImageIO/oiio/blob/master/LICENSE.md

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>

#include <boost/version.hpp>
#if BOOST_VERSION >= 106900
#    include <boost/integer/common_factor_rt.hpp>
using boost::integer::gcd;
#else
#    include <boost/math/common_factor_rt.hpp>
using boost::math::gcd;
#endif

#include <OpenImageIO/platform.h>

#include <OpenEXR/openexr.h>

#ifdef OPENEXR_VERSION_MAJOR
#    define OPENEXR_CODED_VERSION                                    \
        (OPENEXR_VERSION_MAJOR * 10000 + OPENEXR_VERSION_MINOR * 100 \
         + OPENEXR_VERSION_PATCH)
#else
#    define OPENEXR_CODED_VERSION 20000
#endif

#if OPENEXR_CODED_VERSION >= 20400 \
    || __has_include(<OpenEXR/ImfFloatVectorAttribute.h>)
#    define OPENEXR_HAS_FLOATVECTOR 1
#else
#    define OPENEXR_HAS_FLOATVECTOR 0
#endif

#include "imageio_pvt.h"
#include <OpenImageIO/dassert.h>
#include <OpenImageIO/deepdata.h>
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imagebufalgo_util.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/strutil.h>
#include <OpenImageIO/sysutil.h>
#include <OpenImageIO/thread.h>

#define ENABLE_READ_DEBUG_PRINTS 0

OIIO_PLUGIN_NAMESPACE_BEGIN

struct oiioexr_filebuf_struct {
    ImageInput* m_img         = nullptr;
    Filesystem::IOProxy* m_io = nullptr;
};

static void
oiio_exr_error_handler(exr_const_context_t ctxt, exr_result_t code,
                       const char* msg)
{
    void* userdata;
    if (EXR_ERR_SUCCESS == exr_get_user_data(ctxt, &userdata)) {
        if (userdata) {
            oiioexr_filebuf_struct* fb = static_cast<oiioexr_filebuf_struct*>(
                userdata);
            if (fb->m_img) {
                fb->m_img->errorf("EXR Error (%s): %s %s",
                                  (fb->m_io ? fb->m_io->filename().c_str()
                                            : "<unknown>"),
                                  exr_get_error_code_as_string(code), msg);
                return;
            }
        }
    }

    // this should only happen from valid_file check, do we care?
    //std::cerr << "EXR error with no valid context ("
    //          << exr_get_error_code_as_string(code) << "): " << msg
    //          << std::endl;
}

static int64_t
oiio_exr_query_size_func(exr_const_context_t ctxt, void* userdata)
{
    oiioexr_filebuf_struct* fb = static_cast<oiioexr_filebuf_struct*>(userdata);
    if (fb)
        return static_cast<int64_t>(fb->m_io->tell());
    return -1;
}

static int64_t
oiio_exr_read_func(exr_const_context_t ctxt, void* userdata, void* buffer,
                   uint64_t sz, uint64_t offset,
                   exr_stream_error_func_ptr_t error_cb)
{
    oiioexr_filebuf_struct* fb = static_cast<oiioexr_filebuf_struct*>(userdata);
    int64_t nread              = -1;
    if (fb) {
        Filesystem::IOProxy* io = fb->m_io;
        if (io) {
            size_t retval = io->pread(buffer, sz, offset);
            if (retval != size_t(-1)) {
                nread = static_cast<int64_t>(retval);
            } else {
                std::string err = io->error();
                error_cb(ctxt, EXR_ERR_READ_IO,
                         "Could not read from file: \"%s\" (%s)",
                         io->filename().c_str(),
                         err.empty() ? "<unknown error>" : err.c_str());
            }
        }
    }
    return nread;
}

class OpenEXRInput final : public ImageInput {
public:
    OpenEXRInput();
    virtual ~OpenEXRInput() { close(); }
    virtual const char* format_name(void) const override { return "openexr"; }
    virtual int supports(string_view feature) const override
    {
        return (feature == "arbitrary_metadata"
                || feature == "exif"  // Because of arbitrary_metadata
                || feature == "iptc"  // Because of arbitrary_metadata
                || feature == "ioproxy");
    }
    virtual bool valid_file(const std::string& filename) const override;
    virtual bool open(const std::string& name, ImageSpec& newspec,
                      const ImageSpec& config) override;
    virtual bool open(const std::string& name, ImageSpec& newspec) override
    {
        return open(name, newspec, ImageSpec());
    }
    virtual bool close() override;
    virtual int current_subimage(void) const override { return m_subimage; }
    virtual int current_miplevel(void) const override { return m_miplevel; }
    virtual bool seek_subimage(int subimage, int miplevel) override;
    virtual ImageSpec spec(int subimage, int miplevel) override;
    virtual ImageSpec spec_dimensions(int subimage, int miplevel) override;
    virtual bool read_native_scanline(int subimage, int miplevel, int y, int z,
                                      void* data) override;
    virtual bool read_native_scanlines(int subimage, int miplevel, int ybegin,
                                       int yend, int z, void* data) override;
    virtual bool read_native_scanlines(int subimage, int miplevel, int ybegin,
                                       int yend, int z, int chbegin, int chend,
                                       void* data) override;
    virtual bool read_native_tile(int subimage, int miplevel, int x, int y,
                                  int z, void* data) override;
    virtual bool read_native_tiles(int subimage, int miplevel, int xbegin,
                                   int xend, int ybegin, int yend, int zbegin,
                                   int zend, void* data) override;
    virtual bool read_native_tiles(int subimage, int miplevel, int xbegin,
                                   int xend, int ybegin, int yend, int zbegin,
                                   int zend, int chbegin, int chend,
                                   void* data) override;
    virtual bool read_native_deep_scanlines(int subimage, int miplevel,
                                            int ybegin, int yend, int z,
                                            int chbegin, int chend,
                                            DeepData& deepdata) override;
    virtual bool read_native_deep_tiles(int subimage, int miplevel, int xbegin,
                                        int xend, int ybegin, int yend,
                                        int zbegin, int zend, int chbegin,
                                        int chend, DeepData& deepdata) override;

    virtual bool set_ioproxy(Filesystem::IOProxy* ioproxy) override
    {
        OIIO_ASSERT(!m_exr_context);
        m_userdata.m_io = ioproxy;
        return true;
    }

private:
    const ImageSpec& init_part(int subimage, int miplevel);
    struct PartInfo {
        std::atomic_bool initialized;
        ImageSpec spec;
        int topwidth;                        ///< Width of top mip level
        int topheight;                       ///< Height of top mip level
        exr_tile_level_mode_t levelmode;     ///< The level mode
        exr_tile_round_mode_t roundingmode;  ///< Rounding mode
        bool cubeface;       ///< It's a cubeface environment map
        int32_t nmiplevels;  ///< How many MIP levels are there?
        exr_attr_box2i_t top_datawindow;
        exr_attr_box2i_t top_displaywindow;
        std::vector<exr_pixel_type_t> pixeltype;  ///< Imf pixel type for each chan
        std::vector<int> chanbytes;  ///< Size (in bytes) of each channel

        PartInfo()
            : initialized(false)
        {
        }
        PartInfo(const PartInfo& p)
            : initialized((bool)p.initialized)
            , spec(p.spec)
            , topwidth(p.topwidth)
            , topheight(p.topheight)
            , levelmode(p.levelmode)
            , roundingmode(p.roundingmode)
            , cubeface(p.cubeface)
            , nmiplevels(p.nmiplevels)
            , top_datawindow(p.top_datawindow)
            , top_displaywindow(p.top_displaywindow)
            , pixeltype(p.pixeltype)
            , chanbytes(p.chanbytes)
        {
        }
        ~PartInfo() {}
        bool parse_header(OpenEXRInput* in, exr_context_t ctxt, int subimage,
                          int miplevel);
        bool query_channels(OpenEXRInput* in, exr_context_t ctxt, int subimage);
        void compute_mipres(int miplevel, ImageSpec& spec) const;
    };
    friend struct PartInfo;

    // cache of the parsed data
    std::vector<PartInfo> m_parts;  ///< Image parts
    // these are only needed to preserve the concept that you have
    // state of seeking in the file
    int m_subimage;
    int m_miplevel;

    exr_context_t m_exr_context = nullptr;
    oiioexr_filebuf_struct m_userdata;

    std::unique_ptr<Filesystem::IOProxy> m_local_io;
    int m_nsubimages;                   ///< How many subimages are there?
    std::vector<float> m_missingcolor;  ///< Color for missing tile/scanline

    void init()
    {
        m_exr_context    = nullptr;
        m_userdata.m_img = this;
        m_userdata.m_io  = nullptr;
        m_local_io.reset();
        m_missingcolor.clear();
    }

    bool valid_file(const std::string& filename, Filesystem::IOProxy* io) const;

    // Fill in with 'missing' color/pattern.
    bool check_fill_missing(int xbegin, int xend, int ybegin, int yend,
                            int zbegin, int zend, int chbegin, int chend,
                            void* data, stride_t xstride, stride_t ystride);
};



// Obligatory material to make this a recognizeable imageio plugin:
OIIO_PLUGIN_EXPORTS_BEGIN

OIIO_EXPORT ImageInput*
openexr_input_imageio_create()
{
    return new OpenEXRInput;
}

// OIIO_EXPORT int openexr_imageio_version = OIIO_PLUGIN_VERSION; // it's in exroutput.cpp

OIIO_EXPORT const char* openexr_input_extensions[] = { "exr", "sxr", "mxr",
                                                       nullptr };

OIIO_PLUGIN_EXPORTS_END



class StringMap {
    typedef std::map<std::string, std::string> map_t;

public:
    StringMap(void) { init(); }

    const char* operator[](const char* s) const
    {
        map_t::const_iterator i;
        i = m_map.find(s);
        return i == m_map.end() ? s : i->second.c_str();
    }

private:
    map_t m_map;

    void init(void)
    {
        // Ones whose name we change to our convention
        m_map["cameraTransform"]  = "worldtocamera";
        m_map["capDate"]          = "DateTime";
        m_map["comments"]         = "ImageDescription";
        m_map["owner"]            = "Copyright";
        m_map["pixelAspectRatio"] = "PixelAspectRatio";
        m_map["xDensity"]         = "XResolution";
        m_map["expTime"]          = "ExposureTime";
        // Ones we don't rename -- OpenEXR convention matches ours
        m_map["wrapmodes"] = "wrapmodes";
        m_map["aperture"]  = "FNumber";
        // Ones to prefix with openexr:
        m_map["version"]             = "openexr:version";
        m_map["chunkCount"]          = "openexr:chunkCount";
        m_map["maxSamplesPerPixel"]  = "openexr:maxSamplesPerPixel";
        m_map["dwaCompressionLevel"] = "openexr:dwaCompressionLevel";
        // Ones to skip because we handle specially
        m_map["channels"]          = "";
        m_map["compression"]       = "";
        m_map["dataWindow"]        = "";
        m_map["displayWindow"]     = "";
        m_map["envmap"]            = "";
        m_map["tiledesc"]          = "";
        m_map["tiles"]             = "";
        m_map["openexr:lineOrder"] = "";
        m_map["type"]              = "";
        // Ones to skip because we consider them irrelevant

        //        m_map[""] = "";
        // FIXME: Things to consider in the future:
        // preview
        // screenWindowCenter
        // adoptedNeutral
        // renderingTransform, lookModTransform
        // utcOffset
        // longitude latitude altitude
        // focus isoSpeed
    }
};

static StringMap exr_tag_to_oiio_std;


OpenEXRInput::OpenEXRInput() { init(); }



bool
OpenEXRInput::valid_file(const std::string& filename) const
{
    return valid_file(filename, nullptr);
}



bool
OpenEXRInput::valid_file(const std::string& filename,
                         Filesystem::IOProxy* io) const
{
    oiioexr_filebuf_struct udata;
    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;

    cinit.error_handler_fn = &oiio_exr_error_handler;

    // do we always want this?
    std::unique_ptr<Filesystem::IOProxy> localio;
    if (!io) {
        localio.reset(
            new Filesystem::IOFile(filename, Filesystem::IOProxy::Read));
        io = localio.get();
    }

    if (io) {
        udata.m_img
            = nullptr;  // this will silence the errors in the error handler above
        udata.m_io      = io;
        cinit.user_data = &udata;
        cinit.read_fn   = &oiio_exr_read_func;
        cinit.size_fn   = &oiio_exr_query_size_func;
    }

    exr_result_t rv = exr_test_file_header(filename.c_str(), &cinit);
    return (rv == EXR_ERR_SUCCESS);
}



bool
OpenEXRInput::open(const std::string& name, ImageSpec& newspec,
                   const ImageSpec& config)
{
    // First thing's first. See if we're been given an IOProxy. We have to
    // do this before the check for non-exr files, that's why it's here and
    // not where the rest of the configuration hints are handled.
    const ParamValue* param = config.find_attribute("oiio:ioproxy",
                                                    TypeDesc::PTR);
    if (param)
        m_userdata.m_io = param->get<Filesystem::IOProxy*>();

    // Quick check to immediately reject nonexistant or non-exr files.
    //KDTDISABLE quick checks are still file iOPs, let the file open handle this
    //KDTDISABLE if (!m_io && !Filesystem::is_regular(name)) {
    //KDTDISABLE     errorf("Could not open file \"%s\"", name);
    //KDTDISABLE     return false;
    //KDTDISABLE }
    //KDTDISABLE if (!valid_file(name, m_io)) {
    //KDTDISABLE     errorf("\"%s\" is not an OpenEXR file", name);
    //KDTDISABLE     return false;
    //KDTDISABLE }

    // Check any other configuration hints

    // "missingcolor" gives fill color for missing scanlines or tiles.
    if (const ParamValue* m = config.find_attribute("oiio:missingcolor")) {
        if (m->type().basetype == TypeDesc::STRING) {
            // missingcolor as string
            m_missingcolor = Strutil::extract_from_list_string<float>(
                m->get_string());
        } else {
            // missingcolor as numeric array
            int n = m->type().basevalues();
            m_missingcolor.clear();
            m_missingcolor.reserve(n);
            for (int i = 0; i < n; ++i)
                m_missingcolor[i] = m->get_float(i);
        }
    } else {
        // If not passed explicit, is there a global setting?
        std::string mc = OIIO::get_string_attribute("missingcolor");
        if (mc.size())
            m_missingcolor = Strutil::extract_from_list_string<float>(mc);
    }

    // Clear the spec with default constructor
    m_spec = ImageSpec();

    // Establish an input stream. If we weren't given an IOProxy, create one
    // now that just reads from the file.
    if (!m_userdata.m_io) {
        m_userdata.m_io = new Filesystem::IOFile(name,
                                                 Filesystem::IOProxy::Read);
        m_local_io.reset(m_userdata.m_io);
    }
    if (m_userdata.m_io->mode() != Filesystem::IOProxy::Read) {
        // If the proxy couldn't be opened in read mode, try to
        // return an error.
        std::string e = m_userdata.m_io->error();
        errorf("Could not open \"%s\" (%s)", name,
               e.size() ? e : std::string("unknown error"));
        return false;
    }
    m_userdata.m_io->seek(0);

    m_userdata.m_img                = this;
    exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;

    cinit.error_handler_fn = &oiio_exr_error_handler;
    cinit.user_data        = &m_userdata;
    if (m_userdata.m_io) {
        cinit.read_fn = &oiio_exr_read_func;
        cinit.size_fn = &oiio_exr_query_size_func;
    }

    exr_result_t rv = exr_start_read(&m_exr_context, name.c_str(), &cinit);
    if (rv != EXR_ERR_SUCCESS) {
        // the error handler would have already reported the error into us
        m_local_io.reset();
        m_userdata.m_io = nullptr;
        return false;
    }
#if ENABLE_READ_DEBUG_PRINTS
    exr_print_context_info(m_exr_context, 1);
#endif
    rv = exr_get_count(m_exr_context, &m_nsubimages);
    if (rv != EXR_ERR_SUCCESS) {
        m_local_io.reset();
        m_userdata.m_io = nullptr;
        return false;
    }

    m_parts.resize(m_nsubimages);
    m_subimage = -1;
    m_miplevel = -1;

    // Set up for the first subimage ("part"). This will trigger reading
    // information about all the parts.
    bool ok = seek_subimage(0, 0);
    if (ok)
        newspec = m_spec;
    else
        close();
    return ok;
}



const ImageSpec&
OpenEXRInput::init_part(int subimage, int miplevel)
{
    const PartInfo& part(m_parts[subimage]);
    if (!part.initialized) {
        // Only if this subimage hasn't yet been inventoried do we need
        // to lock and seek, but that is only so we don't have to re-look values up
        lock_guard lock(*this);
        if (!part.initialized) {
            if (!seek_subimage(subimage, miplevel)) {
                errorf("Unable to initialize part");
                return part.spec;
            }
        }
    }

    return part.spec;
}



bool
OpenEXRInput::PartInfo::parse_header(OpenEXRInput* in, exr_context_t ctxt,
                                     int subimage, int miplevel)
{
    bool ok = true;
    if (initialized)
        return ok;

    ImageInput::lock_guard lock(*in);
    spec = ImageSpec();

    exr_result_t rv = exr_get_data_window(ctxt, subimage, &top_datawindow);
    if (rv != EXR_ERR_SUCCESS)
        return false;
    rv = exr_get_display_window(ctxt, subimage, &top_displaywindow);
    if (rv != EXR_ERR_SUCCESS)
        return false;
    spec.x           = top_datawindow.min.x;
    spec.y           = top_datawindow.min.y;
    spec.z           = 0;
    spec.width       = top_datawindow.max.x - top_datawindow.min.x + 1;
    spec.height      = top_datawindow.max.y - top_datawindow.min.y + 1;
    spec.depth       = 1;
    topwidth         = spec.width;  // Save top-level mipmap dimensions
    topheight        = spec.height;
    spec.full_x      = top_displaywindow.min.x;
    spec.full_y      = top_displaywindow.min.y;
    spec.full_z      = 0;
    spec.full_width  = top_displaywindow.max.x - top_displaywindow.min.x + 1;
    spec.full_height = top_displaywindow.max.y - top_displaywindow.min.y + 1;
    spec.full_depth  = 1;
    spec.tile_depth  = 1;

    exr_storage_t storage;
    rv = exr_get_storage(ctxt, subimage, &storage);
    if (rv != EXR_ERR_SUCCESS)
        return false;
    uint32_t txsz, tysz;
    if ((storage == EXR_STORAGE_TILED || storage == EXR_STORAGE_DEEP_TILED)
        && EXR_ERR_SUCCESS
               == exr_get_tile_descriptor(ctxt, subimage, &txsz, &tysz,
                                          &levelmode, &roundingmode)) {
        spec.tile_width  = txsz;
        spec.tile_height = tysz;

        int32_t levelsx, levelsy;
        rv = exr_get_tile_levels(ctxt, subimage, &levelsx, &levelsy);
        if (rv != EXR_ERR_SUCCESS)
            return false;
        nmiplevels = std::max(levelsx, levelsy);
    } else {
        spec.tile_width  = 0;
        spec.tile_height = 0;
        levelmode        = EXR_TILE_ONE_LEVEL;
        nmiplevels       = 1;
    }
    if (!query_channels(in, ctxt, subimage))  // also sets format
        return false;

    spec.deep = (storage == EXR_STORAGE_DEEP_TILED
                 || storage == EXR_STORAGE_DEEP_SCANLINE);

    // Unless otherwise specified, exr files are assumed to be linear.
    spec.attribute("oiio:ColorSpace", "Linear");

    if (levelmode != EXR_TILE_ONE_LEVEL)
        spec.attribute("openexr:roundingmode", (int)roundingmode);

    exr_envmap_t envmap;
    rv = exr_attr_get_envmap(ctxt, subimage, "envmap", &envmap);
    if (rv == EXR_ERR_SUCCESS) {
        cubeface = (envmap == EXR_ENVMAP_CUBE);
        spec.attribute("textureformat", cubeface ? "CubeFace Environment"
                                                 : "LatLong Environment");
        // OpenEXR conventions for env maps
        if (!cubeface)
            spec.attribute("oiio:updirection", "y");
        spec.attribute("oiio:sampleborder", 1);
        // FIXME - detect CubeFace Shadow?
    } else {
        cubeface = false;
        if (spec.tile_width && levelmode == EXR_TILE_MIPMAP_LEVELS)
            spec.attribute("textureformat", "Plain Texture");
        // FIXME - detect Shadow
    }

    exr_compression_t comptype;
    rv = exr_get_compression(ctxt, subimage, &comptype);
    if (rv == EXR_ERR_SUCCESS) {
        const char* comp = NULL;
        switch (comptype) {
        case EXR_COMPRESSION_NONE: comp = "none"; break;
        case EXR_COMPRESSION_RLE: comp = "rle"; break;
        case EXR_COMPRESSION_ZIPS: comp = "zips"; break;
        case EXR_COMPRESSION_ZIP: comp = "zip"; break;
        case EXR_COMPRESSION_PIZ: comp = "piz"; break;
        case EXR_COMPRESSION_PXR24: comp = "pxr24"; break;
        case EXR_COMPRESSION_B44: comp = "b44"; break;
        case EXR_COMPRESSION_B44A: comp = "b44a"; break;
        case EXR_COMPRESSION_DWAA: comp = "dwaa"; break;
        case EXR_COMPRESSION_DWAB: comp = "dwab"; break;
        default: break;
        }
        if (comp)
            spec.attribute("compression", comp);
    }

    int32_t attrcount = 0;
    rv                = exr_get_attribute_count(ctxt, subimage, &attrcount);
    if (rv != EXR_ERR_SUCCESS)
        return false;
    for (int32_t i = 0; i < attrcount; ++i) {
        const exr_attribute_t* attr;
        rv = exr_get_attribute_by_index(ctxt, subimage,
                                        EXR_ATTR_LIST_FILE_ORDER, i, &attr);
        if (rv != EXR_ERR_SUCCESS)
            return false;

        const char* oname = exr_tag_to_oiio_std[attr->name];
        // empty name means skip;
        if (!oname || oname[0] == '\0')
            continue;

        switch (attr->type) {
        case EXR_ATTR_BOX2I: {
            TypeDesc bx(TypeDesc::INT, TypeDesc::VEC2, 2);
            spec.attribute(oname, bx, attr->box2i);
            break;
        }

        case EXR_ATTR_BOX2F: {
            TypeDesc bx(TypeDesc::FLOAT, TypeDesc::VEC2, 2);
            spec.attribute(oname, bx, attr->box2f);
            break;
        }

        case EXR_ATTR_CHROMATICITIES: {
            spec.attribute(oname, TypeDesc(TypeDesc::FLOAT, 8),
                           (const float*)attr->chromaticities);
            break;
        }

        case EXR_ATTR_DOUBLE: {
            TypeDesc d(TypeDesc::DOUBLE);
            spec.attribute(oname, d, &(attr->d));
            break;
        }

        case EXR_ATTR_FLOAT: spec.attribute(oname, attr->f); break;
        case EXR_ATTR_FLOAT_VECTOR: {
            TypeDesc fv(TypeDesc::FLOAT, (size_t)attr->floatvector->length);
            spec.attribute(oname, fv, attr->floatvector->arr);

            break;
        }

        case EXR_ATTR_INT: spec.attribute(oname, attr->i); break;
        case EXR_ATTR_KEYCODE:
            // Elevate "keyCode" to smpte:KeyCode
            if (!strcmp(oname, "keyCode"))
                oname = "smpte:KeyCode";
            spec.attribute(oname, TypeKeyCode, attr->keycode);
            break;
        case EXR_ATTR_M33F:
            spec.attribute(oname, TypeMatrix33, attr->m33f);
            break;
        case EXR_ATTR_M33D: {
            TypeDesc m33(TypeDesc::DOUBLE, TypeDesc::MATRIX33);
            spec.attribute(oname, m33, attr->m33d);
            break;
        }

        case EXR_ATTR_M44F:
            spec.attribute(oname, TypeMatrix44, attr->m44f);
            break;
        case EXR_ATTR_M44D: {
            TypeDesc m44(TypeDesc::DOUBLE, TypeDesc::MATRIX44);
            spec.attribute(oname, m44, attr->m44d);
            break;
        }

        case EXR_ATTR_RATIONAL: {
            int32_t n  = attr->rational->num;
            uint32_t d = attr->rational->denom;
            if (d < (1UL << 31)) {
                int r[2];
                r[0] = n;
                r[1] = static_cast<int>(d);
                spec.attribute(oname, TypeRational, r);
            } else {
                int f = static_cast<int>(gcd<long long>(n, d));
                if (f > 1) {
                    int r[2];
                    r[0] = n / f;
                    r[1] = static_cast<int>(d / f);
                    spec.attribute(oname, TypeRational, r);
                } else {
                    // TODO: find a way to allow the client to accept "close" rational values
                    OIIO::debugf(
                        "Don't know what to do with OpenEXR Rational attribute %s with value %d / %u that we cannot represent exactly",
                        oname, n, d);
                }
            }
            break;
        }

        case EXR_ATTR_STRING: spec.attribute(oname, attr->string->str); break;
        case EXR_ATTR_STRING_VECTOR: {
            std::vector<ustring> ustrvec(attr->stringvector->n_strings);
            for (int32_t i = 0, e = attr->stringvector->n_strings; i < e; ++i)
                ustrvec[i] = attr->stringvector->strings[i].str;
            TypeDesc sv(TypeDesc::STRING, ustrvec.size());
            spec.attribute(oname, sv, &ustrvec[0]);
            break;
        }

        case EXR_ATTR_TIMECODE:
            // Elevate "timeCode" to smpte:TimeCode
            if (!strcmp(oname, "timeCode"))
                oname = "smpte:TimeCode";
            spec.attribute(oname, TypeTimeCode, attr->timecode);
            break;
        case EXR_ATTR_V2I: {
            TypeDesc v2(TypeDesc::INT, TypeDesc::VEC2);
            spec.attribute(oname, v2, attr->v2i);
            break;
        }
        case EXR_ATTR_V2F: {
            TypeDesc v2(TypeDesc::FLOAT, TypeDesc::VEC2);
            spec.attribute(oname, v2, attr->v2f);
            break;
        }

        case EXR_ATTR_V2D: {
            TypeDesc v2(TypeDesc::DOUBLE, TypeDesc::VEC2);
            spec.attribute(oname, v2, attr->v2d);
            break;
        }

        case EXR_ATTR_V3I: {
            TypeDesc v3(TypeDesc::INT, TypeDesc::VEC3, TypeDesc::VECTOR);
            spec.attribute(oname, v3, attr->v3i);
            break;
        }
        case EXR_ATTR_V3F: spec.attribute(oname, TypeVector, attr->v3f); break;
        case EXR_ATTR_V3D: {
            TypeDesc v3(TypeDesc::DOUBLE, TypeDesc::VEC3, TypeDesc::VECTOR);
            spec.attribute(oname, v3, attr->v3d);
            break;
        }

        case EXR_ATTR_PREVIEW:
        case EXR_ATTR_OPAQUE:
        case EXR_ATTR_ENVMAP:
        case EXR_ATTR_COMPRESSION:
        case EXR_ATTR_CHLIST:
        case EXR_ATTR_LINEORDER:
        case EXR_ATTR_TILEDESC:
        default:
#if 0
            std::cerr << "  unknown attribute type '" << attr->type_name << "' in name: '" << attr->name << "'" << std::endl;
#endif
            break;
        }
    }

    float aspect   = spec.get_float_attribute("PixelAspectRatio", 0.0f);
    float xdensity = spec.get_float_attribute("XResolution", 0.0f);
    if (xdensity) {
        // If XResolution is found, supply the YResolution and unit.
        spec.attribute("YResolution", xdensity * (aspect ? aspect : 1.0f));
        spec.attribute("ResolutionUnit",
                       "in");  // EXR is always pixels/inch
    }

    // EXR "name" also gets passed along as "oiio:subimagename".
    const char* partname;
    if (exr_get_name(ctxt, subimage, &partname) == EXR_ERR_SUCCESS) {
        if (partname && partname[0] != '\0')
            spec.attribute("oiio:subimagename", partname);
    }

    spec.attribute("oiio:subimages", in->m_nsubimages);

    // Squash some problematic texture metadata if we suspect it's wrong
    pvt::check_texture_metadata_sanity(spec);

    initialized = true;
    return ok;
}



namespace {


static TypeDesc
TypeDesc_from_ImfPixelType(exr_pixel_type_t ptype)
{
    switch (ptype) {
    case EXR_PIXEL_UINT: return TypeDesc::UINT; break;
    case EXR_PIXEL_HALF: return TypeDesc::HALF; break;
    case EXR_PIXEL_FLOAT: return TypeDesc::FLOAT; break;
    default:
        OIIO_ASSERT_MSG(0, "Unknown EXR exr_pixel_type_t %d", int(ptype));
        return TypeUnknown;
    }
}



// Split a full channel name into layer and suffix.
static void
split_name(string_view fullname, string_view& layer, string_view& suffix)
{
    size_t dot = fullname.find_last_of('.');
    if (dot == string_view::npos) {
        suffix = fullname;
        layer  = string_view();
    } else {
        layer  = string_view(fullname.data(), dot + 1);
        suffix = string_view(fullname.data() + dot + 1,
                             fullname.size() - dot - 1);
    }
}


// Used to hold channel information for sorting into canonical order
struct ChanNameHolder {
    string_view fullname;    // layer.suffix
    string_view layer;       // just layer
    string_view suffix;      // just suffix (or the fillname, if no layer)
    int exr_channel_number;  // channel index in the exr (sorted by name)
    int special_index;       // sort order for special reserved names
    exr_pixel_type_t exr_data_type;
    TypeDesc datatype;
    int xSampling;
    int ySampling;

    ChanNameHolder(int n, const exr_attr_chlist_entry_t& exrchan)
        : fullname(exrchan.name.str)
        , exr_channel_number(n)
        , special_index(10000)
        , exr_data_type(exrchan.pixel_type)
        , datatype(TypeDesc_from_ImfPixelType(exrchan.pixel_type))
        , xSampling(exrchan.x_sampling)
        , ySampling(exrchan.y_sampling)
    {
        split_name(fullname, layer, suffix);
    }

    // Compute canoninical channel list sort priority
    void compute_special_index()
    {
        static const char* special[]
            = { "R",    "Red",  "G",  "Green", "B",     "Blue",  "Y",
                "real", "imag", "A",  "Alpha", "AR",    "RA",    "AG",
                "GA",   "AB",   "BA", "Z",     "Depth", "Zback", nullptr };
        for (int i = 0; special[i]; ++i)
            if (Strutil::iequals(suffix, special[i])) {
                special_index = i;
                return;
            }
    }

    // Compute alternate channel sort priority for layers that contain
    // x,y,z.
    void compute_special_index_xyz()
    {
        static const char* special[]
            = { "R",  "Red", "G",  "Green", "B",    "Blue", /* "Y", */
                "X",  "Y",   "Z",  "real",  "imag", "A",     "Alpha", "AR",
                "RA", "AG",  "GA", "AB",    "BA",   "Depth", "Zback", nullptr };
        for (int i = 0; special[i]; ++i)
            if (Strutil::iequals(suffix, special[i])) {
                special_index = i;
                return;
            }
    }

    // Partial sort on layer only
    static bool compare_layer(const ChanNameHolder& a, const ChanNameHolder& b)
    {
        return (a.layer < b.layer);
    }

    // Full sort on layer name, special index, suffix
    static bool compare_cnh(const ChanNameHolder& a, const ChanNameHolder& b)
    {
        if (a.layer < b.layer)
            return true;
        if (a.layer > b.layer)
            return false;
        // Within the same layer
        if (a.special_index < b.special_index)
            return true;
        if (a.special_index > b.special_index)
            return false;
        return a.suffix < b.suffix;
    }
};


// Is the channel name (suffix only) in the list?
static bool
suffixfound(string_view name, cspan<ChanNameHolder> chans)
{
    for (auto& c : chans)
        if (Strutil::iequals(name, c.suffix))
            return true;
    return false;
}


}  // namespace



bool
OpenEXRInput::PartInfo::query_channels(OpenEXRInput* in, exr_context_t ctxt,
                                       int subimage)
{
    OIIO_DASSERT(!initialized);
    bool ok        = true;
    spec.nchannels = 0;
    const exr_attr_chlist_t* chlist;
    exr_result_t rv = exr_get_channels(ctxt, subimage, &chlist);
    if (rv != EXR_ERR_SUCCESS)
        return false;

    std::vector<ChanNameHolder> cnh;
    int c = 0;
    for (; c < chlist->num_channels; ++c) {
        const exr_attr_chlist_entry_t& chan = chlist->entries[c];
        cnh.emplace_back(c, chan);
    }
    spec.nchannels = int(cnh.size());
    if (!spec.nchannels) {
        in->errorf("No channels found");
        return false;
    }

    // First, do a partial sort by layername. EXR should already be in that
    // order, but take no chances.
    std::sort(cnh.begin(), cnh.end(), ChanNameHolder::compare_layer);

    // Now, within each layer, sort by channel name
    for (auto layerbegin = cnh.begin(); layerbegin != cnh.end();) {
        // Identify the subrange that comprises a layer
        auto layerend = layerbegin + 1;
        while (layerend != cnh.end() && layerbegin->layer == layerend->layer)
            ++layerend;

        span<ChanNameHolder> layerspan(&(*layerbegin), layerend - layerbegin);
        // Strutil::printf("layerspan:\n");
        // for (auto& c : layerspan)
        //     Strutil::printf("  %s = %s . %s\n", c.fullname, c.layer, c.suffix);
        if (suffixfound("X", layerspan)
            && (suffixfound("Y", layerspan) || suffixfound("Z", layerspan))) {
            // If "X", and at least one of "Y" and "Z", are found among the
            // channel names of this layer, it must encode some kind of
            // position or normal. The usual sort order will give a weird
            // result. Choose a different sort order to reflect this.
            for (auto& ch : layerspan)
                ch.compute_special_index_xyz();
        } else {
            // Use the usual sort order.
            for (auto& ch : layerspan)
                ch.compute_special_index();
        }
        std::sort(layerbegin, layerend, ChanNameHolder::compare_cnh);

        layerbegin = layerend;  // next set of layers
    }

    // Now we should have cnh sorted into the order that we want to present
    // to the OIIO client.

    spec.format         = TypeDesc::UNKNOWN;
    bool all_one_format = true;
    for (int c = 0; c < spec.nchannels; ++c) {
        spec.channelnames.push_back(cnh[c].fullname);
        spec.channelformats.push_back(cnh[c].datatype);
        spec.format = TypeDesc::basetype_merge(spec.format, cnh[c].datatype);
        pixeltype.push_back(cnh[c].exr_data_type);
        chanbytes.push_back(cnh[c].datatype.size());
        all_one_format &= (cnh[c].datatype == cnh[0].datatype);
        if (spec.alpha_channel < 0
            && (Strutil::iequals(cnh[c].suffix, "A")
                || Strutil::iequals(cnh[c].suffix, "Alpha")))
            spec.alpha_channel = c;
        if (spec.z_channel < 0
            && (Strutil::iequals(cnh[c].suffix, "Z")
                || Strutil::iequals(cnh[c].suffix, "Depth")))
            spec.z_channel = c;
        if (cnh[c].xSampling != 1 || cnh[c].ySampling != 1) {
            ok = false;
            in->errorf(
                "Subsampled channels are not supported (channel \"%s\" has sampling %d,%d).",
                cnh[c].fullname, cnh[c].xSampling, cnh[c].ySampling);
            // FIXME: Some day, we should handle channel subsampling.
        }
    }
    OIIO_DASSERT((int)spec.channelnames.size() == spec.nchannels);
    OIIO_DASSERT(spec.format != TypeDesc::UNKNOWN);
    if (all_one_format)
        spec.channelformats.clear();
    return ok;
}



void
OpenEXRInput::PartInfo::compute_mipres(int miplevel, ImageSpec& spec) const
{
    // Compute the resolution of the requested mip level, and also adjust
    // the "full" size appropriately (based on the exr display window).

    if (levelmode == EXR_TILE_ONE_LEVEL)
        return;  // spec is already correct

    int w = topwidth;
    int h = topheight;
    if (levelmode == EXR_TILE_MIPMAP_LEVELS) {
        for (int m = miplevel; m; --m) {
            if (roundingmode == EXR_TILE_ROUND_DOWN) {
                w = w / 2;
                h = h / 2;
            } else {
                w = (w + 1) / 2;
                h = (h + 1) / 2;
            }
            w = std::max(1, w);
            h = std::max(1, h);
        }
    } else if (levelmode == EXR_TILE_RIPMAP_LEVELS) {
        // FIXME
    } else {
        OIIO_ASSERT_MSG(0, "Unknown levelmode %d", int(levelmode));
    }

    spec.width  = w;
    spec.height = h;
    // N.B. OpenEXR doesn't support data and display windows per MIPmap
    // level.  So always take from the top level.
    exr_attr_box2i_t datawindow    = top_datawindow;
    exr_attr_box2i_t displaywindow = top_displaywindow;
    spec.x                         = datawindow.min.x;
    spec.y                         = datawindow.min.y;
    if (miplevel == 0) {
        spec.full_x      = displaywindow.min.x;
        spec.full_y      = displaywindow.min.y;
        spec.full_width  = displaywindow.max.x - displaywindow.min.x + 1;
        spec.full_height = displaywindow.max.y - displaywindow.min.y + 1;
    } else {
        spec.full_x      = spec.x;
        spec.full_y      = spec.y;
        spec.full_width  = spec.width;
        spec.full_height = spec.height;
    }
    if (cubeface) {
        spec.full_width  = w;
        spec.full_height = w;
    }
}



bool
OpenEXRInput::seek_subimage(int subimage, int miplevel)
{
    if (subimage < 0 || subimage >= m_nsubimages)  // out of range
        return false;

    PartInfo& part(m_parts[subimage]);
    if (!part.initialized) {
        if (!part.parse_header(this, m_exr_context, subimage, miplevel))
            return false;
        part.initialized = true;
    }

    m_subimage = subimage;

    if (miplevel < 0 || miplevel >= part.nmiplevels)  // out of range
        return false;

    m_miplevel = miplevel;
    m_spec     = part.spec;

    if (miplevel == 0 && part.levelmode == EXR_TILE_ONE_LEVEL) {
        return true;
    }

    // Compute the resolution of the requested mip level and adjust the
    // full size fields.
    part.compute_mipres(miplevel, m_spec);

    return true;
}



ImageSpec
OpenEXRInput::spec(int subimage, int miplevel)
{
    ImageSpec ret;
    if (subimage < 0 || subimage >= m_nsubimages)
        return ret;  // invalid
    const PartInfo& part(m_parts[subimage]);
    if (!part.initialized) {
        // Only if this subimage hasn't yet been inventoried do we need
        // to lock and seek.
        lock_guard lock(*this);
        if (!part.initialized) {
            if (!seek_subimage(subimage, miplevel))
                return ret;
        }
    }
    if (miplevel < 0 || miplevel >= part.nmiplevels)
        return ret;  // invalid
    ret = part.spec;
    part.compute_mipres(miplevel, ret);
    return ret;
}



ImageSpec
OpenEXRInput::spec_dimensions(int subimage, int miplevel)
{
    ImageSpec ret;
    if (subimage < 0 || subimage >= m_nsubimages)
        return ret;  // invalid
    const PartInfo& part(m_parts[subimage]);
    if (!part.initialized) {
        // Only if this subimage hasn't yet been inventoried do we need
        // to lock and seek.
        lock_guard lock(*this);
        if (!seek_subimage(subimage, miplevel))
            return ret;
    }
    if (miplevel < 0 || miplevel >= part.nmiplevels)
        return ret;  // invalid
    ret.copy_dimensions(part.spec);
    part.compute_mipres(miplevel, ret);
    return ret;
}



bool
OpenEXRInput::close()
{
    exr_finish(&m_exr_context);
    init();  // Reset to initial state
    return true;
}



bool
OpenEXRInput::read_native_scanline(int subimage, int miplevel, int y, int z,
                                   void* data)
{
    if (!m_exr_context) {
        errorf(
            "called OpenEXRInput::read_native_scanlines without an open file");
        return false;
    }

    const ImageSpec& spec = init_part(subimage, miplevel);

    return read_native_scanlines(subimage, miplevel, y, y + 1, z, 0,
                                 spec.nchannels, data);
}



bool
OpenEXRInput::read_native_scanlines(int subimage, int miplevel, int ybegin,
                                    int yend, int z, void* data)
{
    if (!m_exr_context) {
        errorf(
            "called OpenEXRInput::read_native_scanlines without an open file");
        return false;
    }

    const ImageSpec& spec = init_part(subimage, miplevel);

    return read_native_scanlines(subimage, miplevel, ybegin, yend, z, 0,
                                 spec.nchannels, data);
}



bool
OpenEXRInput::read_native_scanlines(int subimage, int miplevel, int ybegin,
                                    int yend, int /*z*/, int chbegin, int chend,
                                    void* data)
{
    if (!m_exr_context) {
        errorf(
            "called OpenEXRInput::read_native_scanlines without an open file");
        return false;
    }

    // NB: to prevent locking, we use the SUBIMAGE spec, so the mip
    // information is not valid!!!! instead, we will use the library
    // which has an internal thread-safe cache of the sizes if needed
    const ImageSpec& spec = init_part(subimage, miplevel);

    chend = clamp(chend, chbegin + 1, spec.nchannels);

    uint8_t* linedata    = static_cast<uint8_t*>(data);
    size_t pixelbytes    = spec.pixel_bytes(chbegin, chend, true);
    size_t scanlinebytes = (size_t)spec.width * pixelbytes;

    exr_chunk_block_info_t cinfo;
    exr_decode_pipeline_t decoder = { 0 };
    int32_t scansperchunk;
    exr_result_t rv;
    rv = exr_get_scanlines_per_chunk(m_exr_context, subimage, &scansperchunk);
    if (rv != EXR_ERR_SUCCESS)
        return false;

#if ENABLE_READ_DEBUG_PRINTS
    {
        lock_guard lock(*this);

        std::cerr << "exr rns " << m_userdata.m_io->filename() << ":"
                  << subimage << ":" << miplevel << " scans (" << ybegin << '-'
                  << yend << "|" << (yend - ybegin) << ")[" << chbegin << "-"
                  << (chend - 1) << "] -> pb " << pixelbytes << " sb "
                  << scanlinebytes << " spc " << scansperchunk << std::endl;
    }
#endif

    std::vector<uint8_t> fullchunk;
    bool first = true;
    int nlines = scansperchunk;
    for (int y = ybegin; y < yend; y += nlines) {
        uint8_t* cdata = linedata;
        // handle scenario where caller asked us to read a scanline
        // that isn't aligned to a chunk boundary
        int invalid = (y - spec.y) % scansperchunk;
        if (invalid != 0) {
            fullchunk.resize(scanlinebytes * scansperchunk);
            nlines = scansperchunk - invalid;
            cdata  = &fullchunk[0];
            y      = y - invalid;
        } else if (((y + scansperchunk) > yend)
                   && ((y + scansperchunk) < (spec.y + spec.height))) {
            fullchunk.resize(scanlinebytes * scansperchunk);
            nlines = yend - y;
            cdata  = &fullchunk[0];
        } else
            nlines = scansperchunk;

        rv = exr_read_scanline_block_info(m_exr_context, subimage, y, &cinfo);
        if (rv != EXR_ERR_SUCCESS)
            break;
        if (first) {
            rv = exr_decoding_initialize(m_exr_context, subimage, &cinfo,
                                         &decoder);
        } else {
            rv = exr_decoding_update(m_exr_context, subimage, &cinfo, &decoder);
        }
        if (rv != EXR_ERR_SUCCESS)
            break;

        size_t chanoffset = 0;
        for (int c = chbegin; c < chend; ++c) {
            size_t chanbytes  = spec.channelformat(c).size();
            string_view cname = spec.channel_name(c);
            for (int dc = 0; dc < decoder.channel_count; ++dc) {
                exr_coding_channel_info_t& curchan = decoder.channels[dc];
#if ENABLE_READ_DEBUG_PRINTS
                //std::cerr << " looking for " << cname.c_str() << ": dc " << dc
                //          << " curchan " << curchan.channel_name << std::endl;
#endif
                if (cname == curchan.channel_name) {
                    curchan.decode_to_ptr     = cdata + chanoffset;
                    curchan.user_pixel_stride = pixelbytes;
                    curchan.user_line_stride  = scanlinebytes;
                    chanoffset += chanbytes;
#if ENABLE_READ_DEBUG_PRINTS
                    //std::cerr << "   chan " << c << " offset " << chanoffset
                    //          << " stride " << pixelbytes << " linestride "
                    //          << scanlinebytes << std::endl;
#endif
                    break;
                }
            }
        }

        if (first) {
            rv = exr_decoding_choose_default_routines(m_exr_context, subimage,
                                                      &decoder);
            if (rv != EXR_ERR_SUCCESS)
                break;
        }
        rv = exr_decoding_run(m_exr_context, subimage, &decoder);
        if (rv != EXR_ERR_SUCCESS)
            break;

        if (cdata != linedata) {
            y += invalid;
            nlines = std::min(nlines, yend - y);
            memcpy(linedata, cdata + invalid * scanlinebytes,
                   nlines * scanlinebytes);
        }
        first = false;
        linedata += scanlinebytes * nlines;
    }
    exr_decoding_destroy(m_exr_context, &decoder);
    return (rv == EXR_ERR_SUCCESS);
}



bool
OpenEXRInput::read_native_tile(int subimage, int miplevel, int x, int y, int z,
                               void* data)
{
    if (!m_exr_context) {
        errorf("called OpenEXRInput::read_native_tile without an open file");
        return false;
    }

    // NB: to prevent locking, we use the SUBIMAGE spec, so the mip
    // information not valid!!!! instead, we will use the library
    // which has an internal thread-safe cache of the sizes
    const ImageSpec& spec = init_part(subimage, miplevel);
    exr_result_t rv;

    int32_t tilew = spec.tile_width;
    int32_t tileh = spec.tile_height;

    size_t pixelbytes    = spec.pixel_bytes(0, spec.nchannels, true);
    size_t scanlinebytes = size_t(tilew) * pixelbytes;

    int32_t tx = (x - spec.x) / tilew;
    int32_t ty = (y - spec.y) / tileh;

    int32_t levw, levh;
    rv = exr_get_level_sizes(m_exr_context, subimage, miplevel, miplevel, &levw,
                             &levh);
    if (rv != EXR_ERR_SUCCESS)
        return check_fill_missing(x, x + tilew, y, y + tileh, z, z + spec.depth,
                                  0, spec.nchannels, data, pixelbytes,
                                  scanlinebytes);

    exr_chunk_block_info_t cinfo;
    exr_decode_pipeline_t decoder = { 0 };

    rv = exr_read_tile_block_info(m_exr_context, subimage, tx, ty, miplevel,
                                  miplevel, &cinfo);
    if (rv != EXR_ERR_SUCCESS)
        return check_fill_missing(x, std::min(levw, x + tilew), y,
                                  std::min(levh, y + tileh), z, z + spec.depth,
                                  0, spec.nchannels, data, pixelbytes,
                                  scanlinebytes);
    rv = exr_decoding_initialize(m_exr_context, subimage, &cinfo, &decoder);
    if (rv != EXR_ERR_SUCCESS) {
        exr_decoding_destroy(m_exr_context, &decoder);
        return check_fill_missing(x, std::min(levw, x + tilew), y,
                                  std::min(levh, y + tileh), z, z + spec.depth,
                                  0, spec.nchannels, data, pixelbytes,
                                  scanlinebytes);
    }

#if ENABLE_READ_DEBUG_PRINTS
    std::cerr << "openexr rnt single " << m_userdata.m_io->filename() << " si "
              << subimage << " mip " << miplevel << " pos " << x << ' ' << y
              << "\n -> tile " << tx << ", " << ty << ", pixbytes "
              << pixelbytes << " scan " << scanlinebytes << " tilesz " << tilew
              << "x" << tileh << std::endl;
#endif

    uint8_t* cdata    = static_cast<uint8_t*>(data);
    size_t chanoffset = 0;
    for (int c = 0; c < spec.nchannels; ++c) {
        size_t chanbytes  = spec.channelformat(c).size();
        string_view cname = spec.channel_name(c);
        for (int dc = 0; dc < decoder.channel_count; ++dc) {
            exr_coding_channel_info_t& curchan = decoder.channels[dc];
            if (cname == curchan.channel_name) {
                curchan.decode_to_ptr     = cdata + chanoffset;
                curchan.user_pixel_stride = pixelbytes;
                curchan.user_line_stride
                    = scanlinebytes;  //curchan.width * pixelbytes;
                chanoffset += chanbytes;
#if ENABLE_READ_DEBUG_PRINTS
                std::cerr << " chan " << c << " tile " << tx << ", " << ty
                          << ": linestride " << curchan.user_line_stride
                          << " tilesize " << curchan.width << " x "
                          << curchan.height << std::endl;
#endif
                break;
            }
        }
    }
    rv = exr_decoding_choose_default_routines(m_exr_context, subimage,
                                              &decoder);
    if (rv != EXR_ERR_SUCCESS) {
        exr_decoding_destroy(m_exr_context, &decoder);
        return check_fill_missing(x, std::min(levw, x + tilew), y,
                                  std::min(levh, y + tileh), z, z + spec.depth,
                                  0, spec.nchannels, data, pixelbytes,
                                  scanlinebytes);
    }
    rv = exr_decoding_run(m_exr_context, subimage, &decoder);
    exr_decoding_destroy(m_exr_context, &decoder);
    if (rv != EXR_ERR_SUCCESS) {
        return check_fill_missing(x, std::min(levw, x + tilew), y,
                                  std::min(levh, y + tileh), z, z + spec.depth,
                                  0, spec.nchannels, data, pixelbytes,
                                  scanlinebytes);
    }
    return true;
}



bool
OpenEXRInput::read_native_tiles(int subimage, int miplevel, int xbegin,
                                int xend, int ybegin, int yend, int zbegin,
                                int zend, void* data)
{
    if (!m_exr_context) {
        errorf("called OpenEXRInput::read_native_tile without an open file");
        return false;
    }

    const ImageSpec& spec = init_part(subimage, miplevel);

    return read_native_tiles(subimage, miplevel, xbegin, xend, ybegin, yend,
                             zbegin, zend, 0, spec.nchannels, data);
}



bool
OpenEXRInput::read_native_tiles(int subimage, int miplevel, int xbegin,
                                int xend, int ybegin, int yend, int zbegin,
                                int zend, int chbegin, int chend, void* data)
{
    if (!m_exr_context) {
        errorf("called OpenEXRInput::read_native_tile without an open file");
        return false;
    }

    // NB: to prevent locking, we use the SUBIMAGE spec, so the mip
    // information not valid!!!! instead, we will use the library
    // which has an internal thread-safe cache of the sizes
    const ImageSpec& spec = init_part(subimage, miplevel);
    exr_result_t rv       = EXR_ERR_SUCCESS;

    int32_t tilew = spec.tile_width;
    int32_t tileh = spec.tile_height;

    chend          = clamp(chend, chbegin + 1, spec.nchannels);
    int firstxtile = (xbegin - spec.x) / tilew;
    int firstytile = (ybegin - spec.y) / tileh;

    size_t pixelbytes = spec.pixel_bytes(chbegin, chend, true);

    int32_t levw, levh;
    rv = exr_get_level_sizes(m_exr_context, subimage, miplevel, miplevel, &levw,
                             &levh);
    if (rv != EXR_ERR_SUCCESS)
        return check_fill_missing(xbegin, xend, ybegin, yend, zbegin, zend,
                                  chbegin, chend, data, pixelbytes,
                                  size_t(tilew) * pixelbytes
                                      * size_t((xend - xbegin + tilew - 1)
                                               / tilew));

    xend        = std::min(xend, spec.x + levw);
    yend        = std::min(yend, spec.y + levh);
    zend        = std::min(zend, spec.z + spec.depth);
    int nxtiles = (xend - xbegin + tilew - 1) / tilew;
    int nytiles = (yend - ybegin + tileh - 1) / tileh;

    size_t scanlinebytes = size_t(nxtiles) * size_t(tilew) * pixelbytes;

#if ENABLE_READ_DEBUG_PRINTS
    {
        lock_guard lock(*this);

        std::cerr << "exr rnt " << m_userdata.m_io->filename() << ":"
                  << subimage << ":" << miplevel << " (" << xbegin << ' '
                  << xend << ' ' << ybegin << ' ' << yend << "|"
                  << (xend - xbegin) << "x" << (yend - ybegin) << ")["
                  << chbegin << "-" << (chend - 1) << "] -> t " << firstxtile
                  << ", " << firstytile << " n " << nxtiles << ", " << nytiles
                  << " pb " << pixelbytes << " sb " << scanlinebytes << " tsz "
                  << tilew << "x" << tileh << std::endl;
    }

#endif

    exr_chunk_block_info_t cinfo;
    exr_decode_pipeline_t decoder = { 0 };
    bool first                    = true;

    int curytile = firstytile;
    bool retval  = true;
    for (int ty = 0; ty < nytiles; ++ty, ++curytile) {
        int curxtile         = firstxtile;
        uint8_t* tilesetdata = static_cast<uint8_t*>(data);
        tilesetdata += ty * tileh * scanlinebytes;
        for (int tx = 0; tx < nxtiles; ++tx, ++curxtile) {
            uint8_t* curtilestart = tilesetdata + tx * tilew * pixelbytes;
            rv = exr_read_tile_block_info(m_exr_context, subimage, curxtile,
                                          curytile, miplevel, miplevel, &cinfo);
            if (rv != EXR_ERR_SUCCESS) {
                retval &= check_fill_missing(xbegin + tx * tilew,
                                             xbegin + (tx + 1) * tilew,
                                             ybegin + ty * tileh,
                                             ybegin + (ty + 1) * tileh, zbegin,
                                             zend, chbegin, chend, curtilestart,
                                             pixelbytes, scanlinebytes);
                continue;
            }

            if (first) {
                rv = exr_decoding_initialize(m_exr_context, subimage, &cinfo,
                                             &decoder);
            } else {
                rv = exr_decoding_update(m_exr_context, subimage, &cinfo,
                                         &decoder);
            }
            if (rv != EXR_ERR_SUCCESS) {
                retval &= check_fill_missing(xbegin + tx * tilew,
                                             xbegin + (tx + 1) * tilew,
                                             ybegin + ty * tileh,
                                             ybegin + (ty + 1) * tileh, zbegin,
                                             zend, chbegin, chend, curtilestart,
                                             pixelbytes, scanlinebytes);
                continue;
            }
            size_t chanoffset = 0;
            for (int c = chbegin; c < chend; ++c) {
                size_t chanbytes  = spec.channelformat(c).size();
                string_view cname = spec.channel_name(c);
                for (int dc = 0; dc < decoder.channel_count; ++dc) {
                    exr_coding_channel_info_t& curchan = decoder.channels[dc];
                    if (cname == curchan.channel_name) {
                        curchan.decode_to_ptr     = curtilestart + chanoffset;
                        curchan.user_pixel_stride = pixelbytes;
                        curchan.user_line_stride  = scanlinebytes;
                        chanoffset += chanbytes;
#if ENABLE_READ_DEBUG_PRINTS
                        //std::cerr << " chan " << c << " tile " << tx << ", "
                        //          << ty << ": linestride "
                        //          << curchan.user_line_stride << " tilesize "
                        //          << curchan.width << " x " << curchan.height
                        //          << std::endl;
#endif
                        break;
                    }
                }
            }
#if ENABLE_READ_DEBUG_PRINTS
            std::cerr << " -> read " << curxtile << ", " << curytile
                      << ": toff " << tx * tilew * pixelbytes << " tilesize "
                      << cinfo.width << " x " << cinfo.height << " pos "
                      << cinfo.start_x << ", " << cinfo.start_y << std::endl;
#endif

            if (first) {
                rv = exr_decoding_choose_default_routines(m_exr_context,
                                                          subimage, &decoder);
                if (rv != EXR_ERR_SUCCESS) {
                    retval &= check_fill_missing(xbegin + tx * tilew,
                                                 xbegin + (tx + 1) * tilew,
                                                 ybegin + ty * tileh,
                                                 ybegin + (ty + 1) * tileh,
                                                 zbegin, zend, chbegin, chend,
                                                 curtilestart, pixelbytes,
                                                 scanlinebytes);
                    continue;
                }
            }
            first = false;
            rv    = exr_decoding_run(m_exr_context, subimage, &decoder);
            if (rv != EXR_ERR_SUCCESS) {
                retval &= check_fill_missing(xbegin + tx * tilew,
                                             xbegin + (tx + 1) * tilew,
                                             ybegin + ty * tileh,
                                             ybegin + (ty + 1) * tileh, zbegin,
                                             zend, chbegin, chend, curtilestart,
                                             pixelbytes, scanlinebytes);
                continue;
            }
        }
    }
    exr_decoding_destroy(m_exr_context, &decoder);

    return retval;
}



bool
OpenEXRInput::check_fill_missing(int xbegin, int xend, int ybegin, int yend,
                                 int /*zbegin*/, int /*zend*/, int chbegin,
                                 int chend, void* data, stride_t xstride,
                                 stride_t ystride)
{
    if (m_missingcolor.empty())
        return false;
    std::vector<float> missingcolor = m_missingcolor;
    missingcolor.resize(chend, m_missingcolor.back());
    bool stripe = missingcolor[0] < 0.0f;
    if (stripe)
        missingcolor[0] = fabsf(missingcolor[0]);
    for (int y = ybegin; y < yend; ++y) {
        for (int x = xbegin; x < xend; ++x) {
            char* d = (char*)data + (y - ybegin) * ystride
                      + (x - xbegin) * xstride;
            for (int ch = chbegin; ch < chend; ++ch) {
                float v = missingcolor[ch];
                if (stripe && ((x - y) & 8))
                    v = 0.0f;
                TypeDesc cf = m_spec.channelformat(ch);
                if (cf == TypeFloat)
                    *(float*)d = v;
                else if (cf == TypeHalf)
                    *(half*)d = v;
                d += cf.size();
            }
        }
    }
    return true;
}



bool
OpenEXRInput::read_native_deep_scanlines(int subimage, int miplevel, int ybegin,
                                         int yend, int /*z*/, int chbegin,
                                         int chend, DeepData& deepdata)
{
    errorf("Not yet implemented: TODO");
    return false;
}



bool
OpenEXRInput::read_native_deep_tiles(int subimage, int miplevel, int xbegin,
                                     int xend, int ybegin, int yend,
                                     int /*zbegin*/, int /*zend*/, int chbegin,
                                     int chend, DeepData& deepdata)
{
    errorf("Not yet implemented: TODO");
    return false;
}


OIIO_PLUGIN_NAMESPACE_END
