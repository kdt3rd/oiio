/*
  Copyright 2018 Kimball Thurston and the other authors and contributors.
  All Rights Reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:
  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
  * Neither the name of the software's owners nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  (This is the Modified BSD License)
*/

#include <ostream>
#include <vector>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/strutil.h>

OIIO_PLUGIN_NAMESPACE_BEGIN

class FFMPEGPipeOutput final : public ImageOutput {
public:
    FFMPEGPipeOutput ();
    virtual ~FFMPEGPipeOutput ();
    virtual const char * format_name (void) const override { return "ffmpeg_pipe"; }
    virtual int supports (string_view feature) const override;
    virtual bool open (const std::string &name, const ImageSpec &spec,
                       OpenMode mode=Create) override;
    virtual bool close () override;
    virtual bool write_scanline (int y, int z, TypeDesc format,
                                 const void *data, stride_t xstride) override;

private:
    std::ostream *m_obuf = nullptr;
    std::vector<unsigned char> m_scratch;
    size_t m_bytesperline = 0;
    unsigned int m_dither = 0;
    int m_chans = 0;
    int m_bpp = 1; // bytes per pixel
};

// Obligatory material to make this a recognizeable imageio plugin:
OIIO_PLUGIN_EXPORTS_BEGIN

OIIO_EXPORT ImageOutput *ffmpeg_pipe_output_imageio_create () {
    return new FFMPEGPipeOutput;
}

OIIO_EXPORT int ffmpeg_pipe_imageio_version = OIIO_PLUGIN_VERSION;

OIIO_EXPORT const char* ffmpeg_pipe_imageio_library_version () {
    return "1.0";
}

OIIO_EXPORT const char * ffmpeg_pipe_output_extensions[] = {
    "stdout", nullptr
};

OIIO_PLUGIN_EXPORTS_END

////////////////////////////////////////

FFMPEGPipeOutput::FFMPEGPipeOutput ()
{
}

FFMPEGPipeOutput::~FFMPEGPipeOutput ()
{
}

////////////////////////////////////////

int
FFMPEGPipeOutput::supports (string_view feature) const
{
    if (feature == "alpha")
        return true;

    if (feature == "non_filesystem_output")
        return true;

    return false;
}

////////////////////////////////////////

bool FFMPEGPipeOutput::open (const std::string &name, const ImageSpec &spec,
                             OpenMode mode)
{
    m_spec = spec;
    if (m_spec.width < 1 || m_spec.height < 1)
    {
        error ("Image resolution must be at least 1x1, you asked for %d x %d",
               m_spec.width, m_spec.height);
        return false;
    }

    std::vector<string_view> parts;
    Strutil::split (name, parts, ".");
    if ( parts.size() != 2 && parts.size() != 4 )
    {
        error ("Unable to separate pixel format from output pipe destination in filename: '%s', got %d", name.c_str(), int(parts.size()));
        return false;
    }

    m_chans = 3;
    m_bpp = -1;
    // TODO: add more ffmpeg -pix_fmts handled here...
    if ( parts[0] == "rgb24" || parts[0] == "rgb" )
    {
        m_bpp = 1;
        m_chans = 3;
    }
    else if ( parts[0] == "rgb48le" )
    {
        m_bpp = 2;
        m_chans = 3;
    }
    else if ( parts[0] == "rgba" )
    {
        m_bpp = 1;
        m_chans = 4;
    }
    else if ( parts[0] == "rgba64le" )
    {
        m_bpp = 2;
        m_chans = 4;
    }
    else
    {
        error ("No translation for ffmpeg pixel format '%s' has been verified, please add to supported output formats", parts[0].c_str());
        return false;
    }

    if ( m_bpp < 0 )
    {
        error ("FFMPEG pipe stream logic error, missing set of bytes per channel");
        return false;
    }

    if ( m_bpp == 1 && m_spec.format != TypeDesc::UINT8)
    {
        error ("FFMPEG pipe stream requested 8-bit, but output spec is not 8-bit");
        return false;
    }

    if ( m_bpp == 2 && m_spec.format != TypeDesc::UINT16)
    {
        error ("FFMPEG pipe stream requested 16-bit, but output spec is not 16-bit");
        return false;
    }

    if (m_spec.nchannels != m_chans)
    {
        error ("FFMPEG pipe stream requested %d channel output, but output spec is %d", m_chans, m_spec.nchannels);
        return false;
    }

    m_bytesperline = m_spec.width * m_chans * m_bpp;

    if ( parts.back() == "stdout" )
        m_obuf = &std::cout;
    else
    {
        error ("Unknown stream name '%s' specified for pipe output in filename '%s'", parts.back().c_str(), name.c_str());
        return false;
    }

    m_dither = (m_spec.format == TypeDesc::UINT8) ?
                       m_spec.get_int_attribute ("oiio:dither", 0) : 0;

    return true;
}

////////////////////////////////////////

bool FFMPEGPipeOutput::close ()
{
    m_bytesperline = 0;
    m_obuf = nullptr;
    m_dither = 0;

    return true;
}

////////////////////////////////////////

bool FFMPEGPipeOutput::write_scanline (int y, int z, TypeDesc format,
                                       const void *data, stride_t xstride)
{
    m_spec.auto_stride (xstride, format, spec().nchannels);
    data = to_native_scanline (format, data, xstride, m_scratch, m_dither, y, z);
    m_obuf->write ( reinterpret_cast<const char *>( data ), m_bytesperline );
    return static_cast<bool>( (*m_obuf) );
}

OIIO_PLUGIN_NAMESPACE_END


