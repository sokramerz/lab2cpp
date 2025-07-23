#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>
#ifdef _WIN32
    #include <direct.h>
#endif

#pragma pack(push,1)
struct Header {
    char  idLength;
    char  colorMapType;
    char  dataTypeCode;
    short colorMapOrigin;
    short colorMapLength;
    char  colorMapDepth;
    short xOrigin;
    short yOrigin;
    short width;
    short height;
    char  bitsPerPixel;
    char  imageDescriptor;
};
#pragma pack(pop)

struct Pixel { unsigned char b,g,r; };  // TGA order = BGR

struct Img {
    Header h;
    std::vector<Pixel> px;
};

static int  CLAMP(int v){ return v<0?0:(v>255?255:v); }
static unsigned char c255(int v){ return (unsigned char)CLAMP(v); }
static unsigned char f2uc(float f){ return (unsigned char)CLAMP((int)(f+0.5f)); }

static void makeOutputDir(){
#ifdef _WIN32
    _mkdir("output");
#else
    mkdir("output", 0755);
#endif
}

static Img readTGA(const std::string &fname){
    Img im{};
    std::ifstream f(fname.c_str(), std::ios::binary);
    if(!f){ std::cerr << "Couldn't open " << fname << "\n"; exit(1); }

    f.read(reinterpret_cast<char*>(&im.h), sizeof(Header));
    if(!f){ std::cerr << "Bad header in " << fname << "\n"; exit(2); }

    if(im.h.bitsPerPixel != 24 || im.h.dataTypeCode != 2){
        std::cerr << "Unexpected TGA format in " << fname << "\n";
        exit(3);
    }

    size_t count = (size_t)im.h.width * (size_t)im.h.height;
    im.px.resize(count);

    if(im.h.idLength) f.seekg(im.h.idLength, std::ios::cur);

    f.read(reinterpret_cast<char*>(im.px.data()), count*3);
    if(!f){ std::cerr << "Truncated pixel data in " << fname << "\n"; exit(4); }

    return im;
}

static void writeTGA(const Img &im, const std::string &fname){
    std::ofstream f(fname.c_str(), std::ios::binary);
    if(!f){ std::cerr << "Couldn't write to " << fname << "\n"; exit(5); }
    f.write(reinterpret_cast<const char*>(&im.h), sizeof(Header));
    f.write(reinterpret_cast<const char*>(im.px.data()), im.px.size()*3);
}

static Img cloneLike(const Img &ref){
    Img o = ref;
    o.px.assign(ref.px.size(), Pixel{0,0,0});
    return o;
}

enum Mode{ MULTI, SUBTR, SCREEN, OVERLAY };

static Img blend(const Img &top, const Img &bottom, Mode m){
    if(top.h.width!=bottom.h.width || top.h.height!=bottom.h.height){
        std::cerr << "Image sizes mismatch in blend\n";
        exit(42);
    }
    Img out = cloneLike(top);
    for(size_t i=0;i<top.px.size();++i){
        const Pixel &A = top.px[i];     // top
        const Pixel &B = bottom.px[i];  // bottom
        Pixel P{};
        switch(m){
            case MULTI:
                P.r = c255((int)A.r * (int)B.r / 255);
                P.g = c255((int)A.g * (int)B.g / 255);
                P.b = c255((int)A.b * (int)B.b / 255);
                break;
            case SUBTR: // bottom - top
                P.r = c255((int)B.r - (int)A.r);
                P.g = c255((int)B.g - (int)A.g);
                P.b = c255((int)B.b - (int)A.b);
                break;
            case SCREEN: {
                auto S = [](int a,int b){ return 255 - ((255-a)*(255-b))/255; };
                P.r = c255(S(A.r,B.r));
                P.g = c255(S(A.g,B.g));
                P.b = c255(S(A.b,B.b));
            } break;
            case OVERLAY: {
                auto O = [](int a,int b){
                    if(b<128) return (2*a*b)/255;
                    return 255 - (2*(255-a)*(255-b))/255;
                };
                P.r = c255(O(A.r,B.r));
                P.g = c255(O(A.g,B.g));
                P.b = c255(O(A.b,B.b));
            } break;
            default: P = A;
        }
        out.px[i]=P;
    }
    return out;
}

static void addChan(Img &im, int dr,int dg,int db){
    for(auto &p: im.px){
        p.r = c255((int)p.r + dr);
        p.g = c255((int)p.g + dg);
        p.b = c255((int)p.b + db);
    }
}

static void scaleChan(Img &im, float sr,float sg,float sb){
    for(auto &p: im.px){
        p.r = f2uc(p.r * sr);
        p.g = f2uc(p.g * sg);
        p.b = f2uc(p.b * sb);
    }
}

static Img pickChannel(const Img &src, char which){
    Img o = cloneLike(src);
    for(size_t i=0;i<src.px.size();++i){
        unsigned char v = (which=='r')?src.px[i].r : (which=='g')?src.px[i].g : src.px[i].b;
        o.px[i].r = o.px[i].g = o.px[i].b = v;
    }
    return o;
}

static Img combineRGB(const Img &r, const Img &g, const Img &b){
    if(r.h.width!=g.h.width || r.h.width!=b.h.width ||
       r.h.height!=g.h.height || r.h.height!=b.h.height){
        std::cerr << "combineRGB mismatch sizes\n"; exit(99);
    }
    Img o = cloneLike(r);
    for(size_t i=0;i<o.px.size();++i){
        o.px[i].r = r.px[i].r;
        o.px[i].g = g.px[i].g;
        o.px[i].b = b.px[i].b;
    }
    return o;
}

static Img rot180(const Img &src){
    Img o = cloneLike(src);
    o.px = src.px;
    std::reverse(o.px.begin(), o.px.end());
    return o;
}

int main(int argc, char** argv){
    (void)argc; (void)argv;
    makeOutputDir();

    // Part 1
    Img layer1   = readTGA("input/layer1.tga");
    Img pattern1 = readTGA("input/pattern1.tga");
    Img part1    = blend(layer1, pattern1, MULTI);
    writeTGA(part1, "output/part1.tga");

    // Part 2
    Img layer2 = readTGA("input/layer2.tga");
    Img car    = readTGA("input/car.tga");
    Img part2  = blend(layer2, car, SUBTR); // bottom - top
    writeTGA(part2, "output/part2.tga");

    // Part 3
    Img pattern2 = readTGA("input/pattern2.tga");
    Img text     = readTGA("input/text.tga");
    Img tmp3     = blend(layer1, pattern2, MULTI);
    Img part3    = blend(text, tmp3, SCREEN);
    writeTGA(part3, "output/part3.tga");

    // Part 4
    Img circles = readTGA("input/circles.tga");
    Img tmp4    = blend(layer2, circles, MULTI);
    Img part4   = blend(pattern2, tmp4, SUBTR); // subtract pattern2 from tmp4
    writeTGA(part4, "output/part4.tga");

    // Part 5
    Img part5 = blend(layer1, pattern1, OVERLAY);
    writeTGA(part5, "output/part5.tga");

    // Part 6
    Img part6 = car;
    addChan(part6, 0, 200, 0);
    writeTGA(part6, "output/part6.tga");

    // Part 7
    Img part7 = car;
    scaleChan(part7, 4.0f, 1.0f, 0.0f);
    writeTGA(part7, "output/part7.tga");

    // Part 8
    Img rOnly = pickChannel(car, 'r');
    Img gOnly = pickChannel(car, 'g');
    Img bOnly = pickChannel(car, 'b');
    writeTGA(rOnly, "output/part8_r.tga");
    writeTGA(gOnly, "output/part8_g.tga");
    writeTGA(bOnly, "output/part8_b.tga");

    // Part 9
    Img lr = readTGA("input/layer_red.tga");
    Img lg = readTGA("input/layer_green.tga");
    Img lb = readTGA("input/layer_blue.tga");
    Img part9 = combineRGB(lr, lg, lb);
    writeTGA(part9, "output/part9.tga");

    // Part 10
    Img text2 = readTGA("input/text2.tga");
    Img part10 = rot180(text2);
    writeTGA(part10, "output/part10.tga");

    std::cout << "All parts written to ./output\n";
    return 0;
}
