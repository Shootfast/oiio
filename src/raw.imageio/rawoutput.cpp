// Copyright 2021-present Contributors to the OpenImageIO project.
// SPDX-License-Identifier: BSD-3-Clause and Apache-2.0
// https://github.com/OpenImageIO/oiio/blob/master/LICENSE.md

#include <memory>

#include <tiffio.h>

#if defined(USE_LIBRAW)
#include <libraw/libraw.h>
#endif

#include <OpenImageIO/imageio.h>

OIIO_PLUGIN_NAMESPACE_BEGIN

class RawOutput final : public ImageOutput {
public:
    RawOutput() {}
    virtual ~RawOutput() { close(); }
    virtual const char* format_name(void) const override { return "raw"; }
    virtual int supports(string_view feature) const override
    {
        return feature == "displaywindow";
    }
    virtual bool open(const std::string& name, const ImageSpec& spec,
                      OpenMode mode = Create) override;
    virtual bool close() override;
    virtual bool write_scanline(int y, int z, TypeDesc format, const void* data,
                                stride_t xstride) override;
private:
    TIFF* m_tif = nullptr;

    short m_bayerPatternDimensions[2] = {2,2};
    short m_blackLevelDimensions[2] = {2,2};
    float m_colormatrix1[9] = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0,
    };
    float m_colormatrix2[9] = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0,
    };
    float m_asShotNeutral[3] = {1.0, 1.0, 1.0};

    // Move data to scratch area if not already there.
    void* move_to_scratch(std::vector<unsigned char>& scratch, const void* data, size_t nbytes)
    {
        if (scratch.empty() || (const unsigned char*)data != scratch.data()){
            scratch.assign((const unsigned char*)data,
                             (const unsigned char*)data + nbytes);
		}
        return scratch.data();
    }
};

namespace {

    std::string filter_str_to_cfapattern(const std::string& filter){
        std::string cfapattern(4,'\0');

        auto channel_to_cfa_index = [](char c) -> char {
            if      (c == 'R'){ return '\00'; }
            else if (c == 'G'){ return '\01'; }
            else if (c == 'B'){ return '\02'; }
            else if (c == 'C'){ return '\03'; }
            else if (c == 'M'){ return '\04'; }
            else if (c == 'Y'){ return '\05'; }
            else if (c == 'W'){ return '\06'; }
            return '\00';
        };
        if (filter.size() != 4){
            return cfapattern;
        }
        for(size_t i=0; i<4; ++i){
            cfapattern[i] = channel_to_cfa_index(filter[i]);
        }
        return cfapattern;
    }

	void matrix_mul(const float* m33_1, const float* m33_2, float* m33_out){
		m33_out[0] = m33_1[0] * m33_2[0] + m33_1[1] * m33_2[3] + m33_1[2] * m33_2[6];
		m33_out[1] = m33_1[0] * m33_2[1] + m33_1[1] * m33_2[4] + m33_1[2] * m33_2[7];
		m33_out[2] = m33_1[0] * m33_2[2] + m33_1[1] * m33_2[5] + m33_1[2] * m33_2[8];
		m33_out[3] = m33_1[3] * m33_2[0] + m33_1[4] * m33_2[3] + m33_1[5] * m33_2[6];
		m33_out[4] = m33_1[3] * m33_2[1] + m33_1[4] * m33_2[4] + m33_1[5] * m33_2[7];
		m33_out[5] = m33_1[3] * m33_2[2] + m33_1[4] * m33_2[5] + m33_1[5] * m33_2[8];
		m33_out[6] = m33_1[6] * m33_2[0] + m33_1[7] * m33_2[3] + m33_1[8] * m33_2[6];
		m33_out[7] = m33_1[6] * m33_2[1] + m33_1[7] * m33_2[4] + m33_1[8] * m33_2[7];
		m33_out[8] = m33_1[6] * m33_2[2] + m33_1[7] * m33_2[5] + m33_1[8] * m33_2[8];
	}
	const float srgb_to_xyz[9] = {
		0.4384467268, 0.3921736825, 0.1693795907,
		0.2228268127, 0.7086908554, 0.0684823319,
		0.0173143440, 0.1104474947, 0.8722681617
	};

} // namespace

OIIO_PLUGIN_EXPORTS_BEGIN

OIIO_EXPORT ImageOutput*
raw_output_imageio_create()
{
    return new RawOutput;
}

OIIO_EXPORT int raw_imageio_version = OIIO_PLUGIN_VERSION;

OIIO_EXPORT const char*
raw_imageio_library_version()
{
    //TODO The raw exporter is always built, but libraw is not.
    //Need to figure out a good syntax for the version string
#if defined USE_LIBRAW
    return ustring::sprintf("libraw %s", libraw_version()).c_str(); 
#else
    std::string v(TIFFGetVersion());
    v = v.substr(0, v.find('\n'));
    v = Strutil::replace(v, ", ", " ");
    return ustring(v).c_str();
#endif
}

OIIO_EXPORT const char* raw_output_extensions[]
    = { "dng", nullptr };

OIIO_PLUGIN_EXPORTS_END

bool
RawOutput::open(const std::string& name, const ImageSpec& userspec,
                OpenMode mode)
{
    m_spec = userspec;

    // Open the file
#ifdef _WIN32
    std::wstring wname = Strutil::utf8_to_utf16(name);
    m_tif = TIFFOpenW(wname.c_str(), mode == AppendSubimage ? "a" : "w");
#else
    m_tif = TIFFOpen(name.c_str(), mode == AppendSubimage ? "a" : "w");
#endif
    if (!m_tif) {
        errorf("Could not open \"%s\"", name);
        return false;
    }

	// For some reason the following DNG TIFF fields are missing from libtiff
	// so we manually register them
	const TIFFFieldInfo dng_fields[] = {
		{ TIFFTAG_CFAREPEATPATTERNDIM, 2, 2, TIFF_SHORT, FIELD_CUSTOM, 0, 0, (char*)"CFARepeatPatternDim" },
		{ TIFFTAG_CFAPATTERN, -1, -1, TIFF_BYTE, FIELD_CUSTOM, 0, 1, (char*)"CFAPattern" },
		{ TIFFTAG_WHITELEVEL, -2, -1, TIFF_LONG, FIELD_CUSTOM, 0, 1, (char*)"WhiteLevel" },
		{ TIFFTAG_WHITELEVEL, -2, -1, TIFF_SHORT, FIELD_CUSTOM, 0, 1, (char*)"WhiteLevel" },
	};
	TIFFMergeFieldInfo(m_tif, dng_fields, sizeof(dng_fields)/sizeof(dng_fields[0]));

    m_spec.nchannels = 1;
    m_spec.set_format(TypeDesc::UINT16);
    //m_spec.attribute("oiio:BitsPerSample", 10);


    //https://lab.apertus.org/T759
    //https://stackoverflow.com/a/39839854
    TIFFSetField(m_tif, TIFFTAG_DNGVERSION, "\01\04\00\00");
    TIFFSetField(m_tif, TIFFTAG_DNGBACKWARDVERSION, "\01\01\00\00");
    TIFFSetField(m_tif, TIFFTAG_SUBFILETYPE, 0);
    TIFFSetField(m_tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(m_tif, TIFFTAG_MAKE, "");
    TIFFSetField(m_tif, TIFFTAG_MODEL, "");

    TIFFSetField(m_tif, TIFFTAG_IMAGEWIDTH, m_spec.width);
    TIFFSetField(m_tif, TIFFTAG_IMAGELENGTH, m_spec.height);
    TIFFSetField(m_tif, TIFFTAG_BITSPERSAMPLE, 16);
    TIFFSetField(m_tif, TIFFTAG_ROWSPERSTRIP, 1);
    TIFFSetField(m_tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(m_tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
    TIFFSetField(m_tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(m_tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(m_tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);



    // Filter pattern

    TIFFSetField(m_tif, TIFFTAG_CFAREPEATPATTERNDIM, m_bayerPatternDimensions);
    auto cfapattern = filter_str_to_cfapattern(
            m_spec.get_string_attribute("raw:FilterPattern",""));
    TIFFSetField(m_tif, TIFFTAG_CFAPATTERN, 4, cfapattern.c_str());

    TIFFSetField(m_tif, TIFFTAG_CFAPLANECOLOR, 3, "\00\01\02");
    auto cfalayout = m_spec.get_int_attribute("raw:FilterLayout",1);
    TIFFSetField(m_tif, TIFFTAG_CFALAYOUT, cfalayout);

    TIFFSetField(m_tif, TIFFTAG_MAKE, "DNG");
    TIFFSetField(m_tif, TIFFTAG_UNIQUECAMERAMODEL, "DNG");

    // ColorMatrix1 (mandatory)
    const ParamValue* p = m_spec.find_attribute("raw:ColorMatrix1", TypeMatrix33);
	if (p){
		// Have DNG ColorMatrix1 attribute already
        const float* m33 = (const float*)p->data();
        std::copy(m33, m33+9, m_colormatrix1);
	} else {
		p = m_spec.find_attribute("raw:cam_xyz", TypeDesc(TypeDesc::FLOAT, 12));
		if (p){
			// Camera->XYZ matrix we can use
			// A 3x4 matrix but last row is 0 (for cameras we care about)
			const float* cam_xyz = (const float*)p->data();
			std::copy(cam_xyz, cam_xyz+9, m_colormatrix1);
		} else {
			p = m_spec.find_attribute("raw:rgb_cam", TypeDesc(TypeDesc::FLOAT, 12));
			if(p){
				// Camera -> sRGB matrix we can combine with sRGB->XYZ
				// A 4*3 matrix so have to skip every 4th entry for 3x3
				const float* m = (const float*)p->data();
				const float rgb_cam[9] = {
					m[0], m[1], m[2],
					m[4], m[5], m[6],
					m[8], m[9], m[10],
				};
				// Now multiply with sRGB->XYZ matrix
				matrix_mul(rgb_cam, srgb_to_xyz, m_colormatrix1);
			} else {
				// No color information known so just default to sRGB->XYZ
				std::copy(srgb_to_xyz, srgb_to_xyz+9, m_colormatrix1);
			}
		}
	}
    TIFFSetField(m_tif, TIFFTAG_COLORMATRIX1, 9, m_colormatrix1);

    // ColorMatrix2 (optional)
    p = m_spec.find_attribute("raw:ColorMatrix2", TypeMatrix33);
    if (p){
        float* m33 = (float*)p->data();
        std::copy(m33, m33+9, m_colormatrix2);
        TIFFSetField(m_tif, TIFFTAG_COLORMATRIX2, 9, m_colormatrix2);
    }
    // CalibrationMatrix1 (optional)
    p = m_spec.find_attribute("raw:CalibrationMatrix1", TypeMatrix33);
    if (p){
		float calibrationmatrix1[9] = {
			1.0, 0.0, 0.0,
			0.0, 1.0, 0.0,
			0.0, 0.0, 1.0,
		};
        float* m33 = (float*)p->data();
        std::copy(m33, m33+9, calibrationmatrix1);
        TIFFSetField(m_tif, TIFFTAG_CAMERACALIBRATION1, 9, calibrationmatrix1);
    }
    // CalibrationMatrix1 (optional)
    p = m_spec.find_attribute("raw:CalibrationMatrix2", TypeMatrix33);
    if (p){
		float calibrationmatrix2[9] = {
			1.0, 0.0, 0.0,
			0.0, 1.0, 0.0,
			0.0, 0.0, 1.0,
		};
        float* m33 = (float*)p->data();
        std::copy(m33, m33+9, calibrationmatrix2);
        TIFFSetField(m_tif, TIFFTAG_CAMERACALIBRATION2, 9, calibrationmatrix2);
    }
    // CalibrationIlluminant1 (optional)
    p = m_spec.find_attribute("raw:CalibrationIlluminant1", TypeUInt8);
    if (p){
		uint8_t* illuminant = (uint8_t*)p->data();
        TIFFSetField(m_tif, TIFFTAG_CALIBRATIONILLUMINANT1, *illuminant);
    }
    // CalibrationIlluminant2 (optional)
    p = m_spec.find_attribute("raw:CalibrationIlluminant2", TypeUInt8);
    if (p){
		uint8_t* illuminant = (uint8_t*)p->data();
        TIFFSetField(m_tif, TIFFTAG_CALIBRATIONILLUMINANT2, *illuminant);
    }

    // AsShotNeutral (mandatory)
    p = m_spec.find_attribute("raw:asShotNeutral", TypeDesc(TypeDesc::FLOAT, 3));
    if (p){
        float* vec3 = (float*)p->data();
        std::copy(vec3, vec3+3, m_asShotNeutral);
    }
    TIFFSetField(m_tif, TIFFTAG_ASSHOTNEUTRAL, 3, m_asShotNeutral);


    // Active area
    uint32_t active_area[4] = {static_cast<uint32_t>(m_spec.full_y),
                               static_cast<uint32_t>(m_spec.full_x),
                               static_cast<uint32_t>(m_spec.full_y + m_spec.full_height),
                               static_cast<uint32_t>(m_spec.full_x + m_spec.full_width)};
    TIFFSetField(m_tif, TIFFTAG_ACTIVEAREA, active_area);

	// White level (optional)
	p = m_spec.find_attribute("raw:WhiteLevel", TypeDesc(TypeDesc::UINT, 3));
	if (p){
		uint32_t* whitelevel = (uint32_t*)p->data();
		TIFFSetField(m_tif, TIFFTAG_WHITELEVEL, 3, whitelevel);
	}

	// Black level dims (optional)
	p = m_spec.find_attribute("raw:BlackLevelDims", TypeDesc(TypeDesc::UINT, 2));
	if (p){
		uint32_t* dims = (uint32_t*)p->data();
        std::copy(dims, dims+2, m_blackLevelDimensions);
		TIFFSetField(m_tif, TIFFTAG_BLACKLEVELREPEATDIM, m_blackLevelDimensions);
	}

	// Black level (optional)
	size_t entries = m_blackLevelDimensions[0] * m_blackLevelDimensions[1];
	p = m_spec.find_attribute("raw:BlackLevel", TypeDesc(TypeDesc::FLOAT, entries));
	if (p){
		float* blacklevel = (float*)p->data();
		TIFFSetField(m_tif, TIFFTAG_BLACKLEVEL, entries, blacklevel);
	}


	//DefaultScale (optional)
	//TODO: This is pixelAspectRatio

	return true;
}

bool
RawOutput::close()
{
    if (!m_tif){
        return true;
    }

    TIFFClose (m_tif);
    m_tif = nullptr;

    return true;
}

bool
RawOutput::write_scanline(int y, int z, TypeDesc format, const void* data,
                          stride_t xstride)
{
    m_spec.auto_stride(xstride, format, m_spec.nchannels);
    unsigned int dither = 0;
    std::vector<unsigned char> scratch;
    data = to_native_scanline(format, data, xstride, scratch, dither, y, z);
    size_t scanline_vals = m_spec.width;
    data = move_to_scratch(scratch, data, scanline_vals * m_spec.pixel_bytes(true));

    TIFFWriteScanline(m_tif, (tdata_t)data, y, 0);
    return true;
}

OIIO_PLUGIN_NAMESPACE_END

#if 0
    //https://stackoverflow.com/questions/48670841/how-to-produce-an-adobe-dng-image-from-scratch-with-libtiff
    static const short CFARepeatPatternDim[] = { 2,2 }; // 2x2 CFA

    static const float cam_xyz[] = { 
        2.005, -0.771, -0.269, 
        -0.752, 1.688, 0.064, 
        -0.149, 0.283, 0.745 }; // xyz

    static const double sRGB[] = {
        3.6156, -0.8100, -0.0837,
        -0.3094, 1.5500, -0.5439,
        0.0967, -0.4700, 1.9805 }; // sRGB profile

    static const float neutral[] = { 0.807133, 1.0, 0.913289 };

    const char* fname = "C:\\tmp\\dngTest\\output.DNG";

    long sub_offset = 0;
    long white = 0xffff;

    struct stat st;
    struct tm tm;
    char datetime[64];

    FILE *ifp;
    TIFF *tif;

    // ============================================================================
    stat(fname, &st);
    gmtime_s(&tm, &st.st_mtime);
    sprintf(datetime, "%04d:%02d:%02d %02d:%02d:%02d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

    if (!(tif = TIFFOpen("C:\\tmp\\dngTest\\output.dng", "w"))) {
        fclose(ifp);
        exit(-1);
    }

    // Set meta data
    TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 1);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH,   HPIXELS >> 4);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH,  VPIXELS >> 4);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_MAKE, "PointGrey");
    TIFFSetField(tif, TIFFTAG_MODEL, "Grasshopper3 GS3-U3-41C6C");
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_SOFTWARE, "primus_dng");
    TIFFSetField(tif, TIFFTAG_DATETIME, datetime);
    TIFFSetField(tif, TIFFTAG_SUBIFD, 1, &sub_offset);
    TIFFSetField(tif, TIFFTAG_DNGVERSION, "\001\001\0\0");
    TIFFSetField(tif, TIFFTAG_DNGBACKWARDVERSION, "\001\0\0\0");
    TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL, "PointGrey Grasshopper3 GS3-U3-41C6C");
    TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, cam_xyz);
    TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, neutral);
    TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, 21);
    // TIFFSetField(tif, TIFFTAG_ORIGINALRAWFILENAME, fname); TODO enable when clear not to crash

    // All Black Thumbnail
    {
        unsigned char *buf = (unsigned char *)malloc((int)HPIXELS >> 4);
        memset(buf, 0, (int)HPIXELS >> 4);
        for (int row = 0; row < (int)VPIXELS>> 4; row++)
            TIFFWriteScanline(tif, buf, row, 0); // just leave it black, no software uses the built-in preview, builds off real image.
    }

    TIFFWriteDirectory(tif);


    // fprintf(stderr, "Writing TIFF header for main image...\n");
    TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);          // image
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, HPIXELS);     // in pixels
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, VPIXELS);    // in pixels
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);       // int
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, CFARepeatPatternDim);
    TIFFSetField(tif, TIFFTAG_CFAPATTERN, "\001\000\001\002"); // GRGB // 0 = Red, 1 = Green, 2 = Blue, 3 = Cyan, 4 = Magenta, 5 = Yellow, 6 = White 
    //TIFFSetField(tif, TIFFTAG_LINEARIZATIONTABLE, 256, curve); set it off for now
    TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &white);

    fprintf(stderr, "Processing RAW data...\n");

    // bit depth
    unsigned char *pLine = (unsigned char*)malloc(sizeof(unsigned char) * HPIXELS);
    memset(pLine, 0, sizeof(unsigned char) * HPIXELS);
    for (int row = 0; row < VPIXELS; row++)
    {
        TIFFWriteScanline(tif, pLine, row, 0); // this writes a single complete row
    }
    free(pLine);
    TIFFClose(tif);
    return 0;
}
#endif
