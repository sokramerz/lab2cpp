#include <cstdint>
#include <cstring>
#include <cassert>
#include <vector>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <string>
#include <stdexcept>
#include <sys/stat.h>
#ifdef _WIN32
  #include <direct.h>
#endif

// ------------------------ Image container ------------------------
struct Image {
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<uint8_t> pixels; // B,G,R order, row-major, bottom-left logical origin
    static constexpr size_t PIXEL_SIZE = 3;

    uint8_t* px(int x, int y){
        assert(x >= 0 && x < width && y >= 0 && y < height);
        return pixels.data() + ((height - 1 - y) * width + x) * PIXEL_SIZE;
    }
    const uint8_t* px(int x, int y) const{
        assert(x >= 0 && x < width && y >= 0 && y < height);
        return pixels.data() + ((height - 1 - y) * width + x) * PIXEL_SIZE;
    }
};

// ------------------------ Math helpers ------------------------
namespace ColorMath {
    inline uint8_t clampByte(int v){ return v < 0 ? 0 : (v > 255 ? 255 : v); }
}

// ------------------------ TGA I/O ------------------------
namespace TGA {
#pragma pack(push, 1)
    struct Header {
        uint8_t  idLength;
        uint8_t  colorMapType;
        uint8_t  dataTypeCode;
        uint16_t colorMapOrigin;
        uint16_t colorMapLength;
        uint8_t  colorMapDepth;
        uint16_t xOrigin;
        uint16_t yOrigin;
        uint16_t width;
        uint16_t height;
        uint8_t  bitsPerPixel;
        uint8_t  imageDescriptor;
    };
#pragma pack(pop)

    Image load(const std::string& path){
        std::ifstream file(path, std::ios::binary);
        if(!file) throw std::runtime_error("Can't open TGA: " + path);

        Header hdr{};
        file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if(hdr.dataTypeCode != 2)  throw std::runtime_error(path + ": Need uncompressed RGB (type 2)");
        if(hdr.bitsPerPixel != 24) throw std::runtime_error(path + ": Need 24-bit RGB");

        if(hdr.idLength) file.seekg(hdr.idLength, std::ios::cur);

        Image img;
        img.width  = hdr.width;
        img.height = hdr.height;
        img.pixels.resize(img.width * img.height * Image::PIXEL_SIZE);
        file.read(reinterpret_cast<char*>(img.pixels.data()), img.pixels.size());
        if(!file) throw std::runtime_error(path + ": Truncated pixel data");

        // Flip if file stored top-left
        if(hdr.imageDescriptor & 0x20){
            const size_t rowBytes = img.width * Image::PIXEL_SIZE;
            std::vector<uint8_t> tmp(img.pixels.size());
            for(int y=0; y<img.height; ++y){
                std::memcpy(tmp.data() + y*rowBytes,
                            img.pixels.data() + (img.height-1-y)*rowBytes,
                            rowBytes);
            }
            img.pixels.swap(tmp);
        }
        return img;
    }

    void save(const Image& img, const std::string& path){
        std::ofstream file(path, std::ios::binary);
        if(!file) throw std::runtime_error("Can't write TGA: " + path);

        Header hdr{};
        hdr.dataTypeCode    = 2;      // uncompressed truecolor
        hdr.width           = img.width;
        hdr.height          = img.height;
        hdr.bitsPerPixel    = 24;
        hdr.imageDescriptor = 0x00;   // bottom-left origin

        file.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

        const size_t rowBytes = img.width * Image::PIXEL_SIZE;
        for(int y = 0; y < img.height; ++y){
            const uint8_t* row = img.pixels.data() + (img.height - 1 - y) * rowBytes;
            file.write(reinterpret_cast<const char*>(row), rowBytes);
        }
        if(!file) throw std::runtime_error("Write failed: " + path);
    }
}

// ------------------------ Blend ops ------------------------
namespace Blend {
    enum Mode { ADD, SUBTRACT, MULTIPLY, SCREEN, OVERLAY };

    inline void blendPixel(Mode mode, const uint8_t* base, const uint8_t* over, uint8_t* out){
        switch(mode){
            case ADD:
                for(int i=0;i<3;++i){ int s = base[i] + over[i]; out[i] = (s > 255) ? 255 : s; }
                break;
            case SUBTRACT:
                for(int i=0;i<3;++i){ out[i] = (base[i] > over[i]) ? (base[i] - over[i]) : 0; }
                break;
            case MULTIPLY:
                for(int i=0;i<3;++i){ out[i] = (base[i] * over[i]) / 255; }
                break;
            case SCREEN:
                for(int i=0;i<3;++i){ int a = base[i], b = over[i]; out[i] = 255 - ((255 - a)*(255 - b))/255; }
                break;
            case OVERLAY:
                for(int i=0;i<3;++i){ int a = base[i], b = over[i];
                    out[i] = (a < 128) ? (2 * a * b) / 255
                                       : 255 - (2 * (255 - a) * (255 - b)) / 255;
                }
                break;
        }
    }

    Image apply(const Image& bottom, const Image& top, Mode mode){
        if(bottom.width!=top.width || bottom.height!=top.height)
            throw std::runtime_error("Blend size mismatch");
        Image out; out.width=bottom.width; out.height=bottom.height;
        out.pixels.resize(out.width*out.height*Image::PIXEL_SIZE);
        const uint8_t* b = bottom.pixels.data();
        const uint8_t* t = top.pixels.data();
        uint8_t*       o = out.pixels.data();
        size_t count = out.width*out.height;
        for(size_t i=0;i<count;++i){ blendPixel(mode,b,t,o); b+=3; t+=3; o+=3; }
        return out;
    }
}

// ------------------------ EXTRA HELPERS (Parts 6–10) ------------------------
static void addToChannel(Image& img, int idx, int delta){
    for(size_t i=0;i<img.pixels.size(); i+=Image::PIXEL_SIZE){
        int v = img.pixels[i+idx] + delta;
        img.pixels[i+idx] = ColorMath::clampByte(v);
    }
}
static void scaleChannel(Image& img, int idx, float factor){
    for(size_t i=0;i<img.pixels.size(); i+=Image::PIXEL_SIZE){
        int v = static_cast<int>(img.pixels[i+idx] * factor + 0.5f);
        img.pixels[i+idx] = ColorMath::clampByte(v);
    }
}
static void splitRGB(const Image& src, Image& r, Image& g, Image& b){
    auto prep=[&](Image& dst){ dst.width=src.width; dst.height=src.height; dst.pixels.resize(src.pixels.size()); };
    prep(r); prep(g); prep(b);
    for(size_t i=0;i<src.pixels.size(); i+=3){
        uint8_t B = src.pixels[i+0], G = src.pixels[i+1], R = src.pixels[i+2];
        r.pixels[i+0]=r.pixels[i+1]=r.pixels[i+2]=R;
        g.pixels[i+0]=g.pixels[i+1]=g.pixels[i+2]=G;
        b.pixels[i+0]=b.pixels[i+1]=b.pixels[i+2]=B;
    }
}
static Image combineRGB(const Image& r, const Image& g, const Image& b){
    if(r.width!=g.width || r.width!=b.width || r.height!=g.height || r.height!=b.height)
        throw std::runtime_error("combine: size mismatch");
    Image out; out.width=r.width; out.height=r.height; out.pixels.resize(out.width*out.height*3);
    for(size_t i=0;i<out.pixels.size(); i+=3){
        out.pixels[i+2] = r.pixels[i]; // R
        out.pixels[i+1] = g.pixels[i]; // G
        out.pixels[i+0] = b.pixels[i]; // B
    }
    return out;
}
static Image rotate180(const Image& src){
    Image out; out.width=src.width; out.height=src.height; out.pixels.resize(src.pixels.size());
    size_t pix = src.width * src.height;
    for(size_t p=0; p<pix; ++p){
        size_t q = pix - 1 - p;
        out.pixels[p*3+0] = src.pixels[q*3+0];
        out.pixels[p*3+1] = src.pixels[q*3+1];
        out.pixels[p*3+2] = src.pixels[q*3+2];
    }
    return out;
}

// ------------------------ Tests ------------------------
namespace Tests {
    void check(bool c, const std::string& msg){ if(!c) throw std::runtime_error("TEST FAIL: " + msg); }
    size_t countDiff(const Image& a, const Image& b){
        if(a.width!=b.width || a.height!=b.height) return SIZE_MAX;
        size_t d=0; for(size_t i=0;i<a.pixels.size();++i) if(a.pixels[i]!=b.pixels[i]) ++d; return d;
    }
    void runAll(){
        std::cout << "Running tests...\n";
        // pixel addressing
        {
            Image img; img.width=3; img.height=3; img.pixels.resize(27,0);
            img.px(0,0)[0]=10; img.px(2,2)[0]=20;
            check(img.px(0,0)[0]==10, "bottom-left pixel");
            check(img.px(2,2)[0]==20, "top-right pixel");
        }
        // blend math
        {
            uint8_t b[3]={100,150,200}, o[3]={50,50,50}, out[3];
            Blend::blendPixel(Blend::ADD,     b,o,out); check(out[0]==150 && out[1]==200 && out[2]==250, "add");
            Blend::blendPixel(Blend::SUBTRACT,b,o,out); check(out[0]==50  && out[1]==100 && out[2]==150, "sub");
            uint8_t g50[3]={128,128,128};
            Blend::blendPixel(Blend::MULTIPLY,b,g50,out); check(out[0]==50 && out[1]==75 && out[2]==100, "mult 50% gray");
        }
        // sat limits
        {
            int big = 200+100; check((big>255?255:big)==255, "add clip");
            int neg = 50-100;  check((neg<0?0:neg)==0,       "sub floor");
        }
        // round-trip tiny file
        {
            uint8_t px[12]={0,0,255,  0,255,0,  255,0,0,  128,128,128};
            Image t; t.width=2; t.height=2; t.pixels.assign(px,px+12);
            TGA::save(t,"test_2x2.tga");
            Image l = TGA::load("test_2x2.tga");
            check(l.px(0,0)[2]==255, "red at (0,0)");
            check(l.px(1,1)[0]==128 && l.px(1,1)[1]==128, "gray at (1,1)");
        }
        std::cout << "All tests passed\n";
    }
}

// ------------------------ Misc ------------------------
static void ensureOutputDir(){
#ifdef _WIN32
    _mkdir("output");
#else
    mkdir("output",0755);
#endif
}

static void usage(const char* p){
    std::cerr << "Usage:\n"
              << "  "<<p<<" test\n"
              << "  "<<p<<" <blend> <base> <overlay> <out>    (blend: add|subtract|multiply|screen|overlay)\n"
              << "  "<<p<<" addch   <r|g|b> <delta>  <in> <out>\n"
              << "  "<<p<<" scalech <r|g|b> <factor> <in> <out>\n"
              << "  "<<p<<" split   <in> <out_prefix>\n"
              << "  "<<p<<" combine <r.tga> <g.tga> <b.tga> <out>\n"
              << "  "<<p<<" rot180  <in> <out>\n"
              << "  "<<p<<" pixdiff <a.tga> <b.tga>\n"
              << "  "<<p<<" runall  (expects input/ & output/)\n";
}

// Helper to map channel char to BGR index
enum { CH_B=0, CH_G=1, CH_R=2 };
static int chanIndex(char c){ return (c=='b'||c=='B')?CH_B : (c=='g'||c=='G')?CH_G : CH_R; }

// Generate all assignment parts in one go
static void doRunAll(){
    ensureOutputDir();
    // 1
    TGA::save( Blend::apply(TGA::load("input/layer1.tga"), TGA::load("input/pattern1.tga"), Blend::MULTIPLY), "output/part1.tga" );
    // 2
    TGA::save( Blend::apply(TGA::load("input/layer2.tga"), TGA::load("input/car.tga"),      Blend::SUBTRACT), "output/part2.tga" );
    // 3
    {
        Image tmp = Blend::apply(TGA::load("input/layer1.tga"), TGA::load("input/pattern2.tga"), Blend::MULTIPLY);
        TGA::save( Blend::apply(TGA::load("input/text.tga"), tmp, Blend::SCREEN), "output/part3.tga" );
    }
    // 4
    {
        Image tmp = Blend::apply(TGA::load("input/layer2.tga"), TGA::load("input/circles.tga"), Blend::MULTIPLY);
        TGA::save( Blend::apply(TGA::load("input/pattern2.tga"), tmp, Blend::SUBTRACT), "output/part4.tga" );
    }
    // 5
    TGA::save( Blend::apply(TGA::load("input/layer1.tga"), TGA::load("input/pattern1.tga"), Blend::OVERLAY),  "output/part5.tga" );
    // 6
    {
        Image img = TGA::load("input/car.tga"); addToChannel(img, CH_G, 200); TGA::save(img, "output/part6.tga");
    }
    // 7
    {
        Image img = TGA::load("input/car.tga"); scaleChannel(img, CH_R, 4.0f); scaleChannel(img, CH_B, 0.0f); TGA::save(img, "output/part7.tga");
    }
    // 8
    {
        Image src = TGA::load("input/car.tga"); Image r,g,b; splitRGB(src,r,g,b);
        TGA::save(r, "output/part8_r.tga"); TGA::save(g, "output/part8_g.tga"); TGA::save(b, "output/part8_b.tga");
    }
    // 9
    {
        Image r = TGA::load("input/layer_red.tga");
        Image g = TGA::load("input/layer_green.tga");
        Image b = TGA::load("input/layer_blue.tga");
        Image out = combineRGB(r,g,b);
        TGA::save(out, "output/part9.tga");
    }
    // 10
    {
        Image t2 = TGA::load("input/text2.tga");
        Image r180 = rotate180(t2);
        TGA::save(r180, "output/part10.tga");
    }
    std::cout << "All parts generated in ./output\n";
}

int main(int argc, char* argv[]){
    try{
        if(argc < 2){ usage(argv[0]); return 1; }
        std::string cmd = argv[1];

        if(cmd == "test"){ Tests::runAll(); return 0; }
        if(cmd == "runall"){ doRunAll(); return 0; }
        if(cmd == "pixdiff"){
            if(argc!=4){ usage(argv[0]); return 1; }
            Image A = TGA::load(argv[2]);
            Image B = TGA::load(argv[3]);
            size_t d = Tests::countDiff(A,B);
            if(d==0) std::cout << "MATCH\n";
            else     std::cout << "DIFFS=" << d << "\n";
            return 0;
        }

        // Blend commands
        if(cmd=="add"||cmd=="subtract"||cmd=="multiply"||cmd=="screen"||cmd=="overlay"){
            if(argc!=5){ usage(argv[0]); return 1; }
            Blend::Mode m = (cmd=="add")?Blend::ADD:(cmd=="subtract")?Blend::SUBTRACT:
                             (cmd=="multiply")?Blend::MULTIPLY:(cmd=="screen")?Blend::SCREEN:Blend::OVERLAY;
            std::cout << "Loading base: "   << argv[2] << "\n";
            Image base = TGA::load(argv[2]);
            std::cout << "Loading overlay: "<< argv[3] << "\n";
            Image over = TGA::load(argv[3]);
            std::cout << "Blending: "       << cmd     << "\n";
            Image out = Blend::apply(base, over, m);
            std::cout << "Saving: "         << argv[4] << "\n";
            TGA::save(out, argv[4]);
            return 0;
        }

        if(cmd=="addch"){
            if(argc!=6){ usage(argv[0]); return 1; }
            int idx = chanIndex(argv[2][0]);
            int delta = std::stoi(argv[3]);
            Image img = TGA::load(argv[4]);
            addToChannel(img, idx, delta);
            TGA::save(img, argv[5]);
            return 0;
        }
        if(cmd=="scalech"){
            if(argc!=6){ usage(argv[0]); return 1; }
            int idx = chanIndex(argv[2][0]);
            float f = std::stof(argv[3]);
            Image img = TGA::load(argv[4]);
            scaleChannel(img, idx, f);
            TGA::save(img, argv[5]);
            return 0;
        }
        if(cmd=="split"){
            if(argc!=4){ usage(argv[0]); return 1; }
            Image src = TGA::load(argv[2]);
            Image r,g,b; splitRGB(src,r,g,b);
            TGA::save(r, std::string(argv[3]) + "_r.tga");
            TGA::save(g, std::string(argv[3]) + "_g.tga");
            TGA::save(b, std::string(argv[3]) + "_b.tga");
            return 0;
        }
        if(cmd=="combine"){
            if(argc!=6){ usage(argv[0]); return 1; }
            Image r = TGA::load(argv[2]);
            Image g = TGA::load(argv[3]);
            Image b = TGA::load(argv[4]);
            Image out = combineRGB(r,g,b);
            TGA::save(out, argv[5]);
            return 0;
        }
        if(cmd=="rot180"){
            if(argc!=4){ usage(argv[0]); return 1; }
            Image src = TGA::load(argv[2]);
            Image out = rotate180(src);
            TGA::save(out, argv[3]);
            return 0;
        }

        usage(argv[0]);
        return 1;
    }catch(const std::exception& e){
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
