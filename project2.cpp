#include <cstdint>
#include <cstring>
#include <cassert>
#include <vector>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <string>
#include <stdexcept>

// TGA image container - just the data, no fancy methods
struct Image {
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<uint8_t> pixels;  // BGR triplets, bottom-left origin
    
    static constexpr size_t PIXEL_SIZE = 3;  // BGR only, no alpha for this project
    
    // Direct pixel access - I keep bottom-left origin everywhere for sanity
    uint8_t* px(int x, int y) {
        assert(x >= 0 && x < width && y >= 0 && y < height);
        return pixels.data() + ((height - 1 - y) * width + x) * PIXEL_SIZE;
    }
    
    const uint8_t* px(int x, int y) const {
        assert(x >= 0 && x < width && y >= 0 && y < height);
        return pixels.data() + ((height - 1 - y) * width + x) * PIXEL_SIZE;
    }
};

// Quick color math - inlined where it makes sense
namespace ColorMath {
    inline uint8_t denorm(float v) {
        return static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, v * 255.0f + 0.5f)));
    }
}

// TGA loading/saving - only handles uncompressed RGB because that's all we need
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
    
    Image load(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Can't open TGA: " + path);
        }
        
        Header hdr;
        file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        
        if (hdr.dataTypeCode != 2) {
            throw std::runtime_error(path + ": Need uncompressed RGB (type 2), got type " +
                                     std::to_string(hdr.dataTypeCode));
        }
        if (hdr.bitsPerPixel != 24) {
            throw std::runtime_error(path + ": Need 24-bit RGB, got " +
                                     std::to_string(hdr.bitsPerPixel) + "-bit");
        }
        
        if (hdr.idLength > 0) {
            file.seekg(hdr.idLength, std::ios::cur);
        }
        
        Image img;
        img.width = hdr.width;
        img.height = hdr.height;
        img.pixels.resize(img.width * img.height * Image::PIXEL_SIZE);
        
        file.read(reinterpret_cast<char*>(img.pixels.data()), img.pixels.size());
        if (!file) {
            throw std::runtime_error(path + ": Truncated file, couldn't read all pixels");
        }
        
        // If top-left origin, flip to bottom-left
        if (hdr.imageDescriptor & 0x20) {
            std::vector<uint8_t> temp(img.pixels.size());
            size_t rowBytes = img.width * Image::PIXEL_SIZE;
            for (int y = 0; y < img.height; ++y) {
                std::memcpy(temp.data() + y * rowBytes,
                            img.pixels.data() + (img.height - 1 - y) * rowBytes,
                            rowBytes);
            }
            img.pixels.swap(temp);
        }
        
        return img;
    }
    
    void save(const Image& img, const std::string& path) {
        std::ofstream file(path, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Can't write TGA: " + path);
        }
        
        Header hdr = {};
        hdr.dataTypeCode    = 2;   // uncompressed RGB
        hdr.width           = img.width;
        hdr.height          = img.height;
        hdr.bitsPerPixel    = 24;
        hdr.imageDescriptor = 0x20;  // top-left origin
        
        file.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        file.write(reinterpret_cast<const char*>(img.pixels.data()), img.pixels.size());
        
        if (!file) {
            throw std::runtime_error("Write failed: " + path);
        }
    }
}

// Blend operations - keeping it simple with an enum + switch
namespace Blend {
    enum Mode {
        ADD,
        SUBTRACT,
        MULTIPLY,
        SCREEN,
        OVERLAY
    };
    
    inline void blendPixel(Mode mode, const uint8_t* base, const uint8_t* over, uint8_t* out) {
        switch (mode) {
        case ADD:
            for (int i = 0; i < 3; ++i) {
                int sum = base[i] + over[i];
                out[i] = (sum > 255) ? 255 : sum;
            }
            break;
        case SUBTRACT:
            for (int i = 0; i < 3; ++i) {
                out[i] = (base[i] > over[i]) ? base[i] - over[i] : 0;
            }
            break;
        case MULTIPLY:
            for (int i = 0; i < 3; ++i) {
                out[i] = ColorMath::denorm((base[i]/255.0f) * (over[i]/255.0f));
            }
            break;
        case SCREEN:
            for (int i = 0; i < 3; ++i) {
                float b = base[i]/255.0f;
                float o = over[i]/255.0f;
                out[i] = ColorMath::denorm(1.0f - (1.0f - b) * (1.0f - o));
            }
            break;
        case OVERLAY: {
                float b0 = base[0]/255.0f, b1 = base[1]/255.0f, b2 = base[2]/255.0f;
                float o0 = over[0]/255.0f, o1 = over[1]/255.0f, o2 = over[2]/255.0f;
                out[0] = ColorMath::denorm(b0 < 0.5f ? (2.0f * b0 * o0) :
                                           (1.0f - 2.0f * (1.0f - b0) * (1.0f - o0)));
                out[1] = ColorMath::denorm(b1 < 0.5f ? (2.0f * b1 * o1) :
                                           (1.0f - 2.0f * (1.0f - b1) * (1.0f - o1)));
                out[2] = ColorMath::denorm(b2 < 0.5f ? (2.0f * b2 * o2) :
                                           (1.0f - 2.0f * (1.0f - b2) * (1.0f - o2)));
            } break;
        }
    }
    
    Image apply(const Image& bottom, const Image& top, Mode mode) {
        if (bottom.width != top.width || bottom.height != top.height) {
            static const char* modeName[] = {"add", "subtract", "multiply", "screen", "overlay"};
            throw std::runtime_error("Blend '" + std::string(modeName[mode]) +
                "' failed: base=" + std::to_string(bottom.width) + "x" +
                std::to_string(bottom.height) + " vs overlay=" +
                std::to_string(top.width) + "x" + std::to_string(top.height));
        }
        
        Image result;
        result.width = bottom.width;
        result.height = bottom.height;
        result.pixels.resize(bottom.width * bottom.height * Image::PIXEL_SIZE);
        
        const uint8_t* basePtr = bottom.pixels.data();
        const uint8_t* overPtr = top.pixels.data();
        uint8_t* outPtr = result.pixels.data();
        
        size_t pixelCount = bottom.width * bottom.height;
        for (size_t i = 0; i < pixelCount; ++i) {
            blendPixel(mode, basePtr, overPtr, outPtr);
            basePtr += Image::PIXEL_SIZE;
            overPtr += Image::PIXEL_SIZE;
            outPtr  += Image::PIXEL_SIZE;
        }
        
        return result;
    }
}

#ifdef DEBUG_TOOLS
namespace Debug {
    Image toGrayscale(const Image& src) {
        Image gray;
        gray.width = src.width;
        gray.height = src.height;
        gray.pixels.resize(src.width * src.height * Image::PIXEL_SIZE);
        
        const uint8_t* in = src.pixels.data();
        uint8_t* out = gray.pixels.data();
        
        for (size_t i = 0; i < gray.width * gray.height; ++i) {
            uint8_t lum = ColorMath::denorm(
                0.114f * (in[0]/255.0f) +
                0.587f * (in[1]/255.0f) +
                0.299f * (in[2]/255.0f)
            );
            out[0] = out[1] = out[2] = lum;
            in  += Image::PIXEL_SIZE;
            out += Image::PIXEL_SIZE;
        }
        return gray;
    }
}
#endif

// Quick test suite
namespace Tests {
    void check(bool cond, const std::string& what) {
        if (!cond) throw std::runtime_error("TEST FAIL: " + what);
    }
    
    size_t countDiff(const Image& a, const Image& b){
        if(a.width!=b.width || a.height!=b.height) return SIZE_MAX;
        size_t diffs = 0;
        for(size_t i=0;i<a.pixels.size();++i)
            if(a.pixels[i]!=b.pixels[i]) ++diffs;
        return diffs;
    }

    void runAll() {
        std::cout << "Running tests...\n";
        
        // Test 1: pixel addressing stays bottom-left after flips
        {
            Image img;
            img.width = 3;
            img.height = 3;
            img.pixels.resize(27, 0);
            img.px(0, 0)[0] = 10;  // bottom-left
            img.px(2, 2)[0] = 20;  // top-right
            check(img.px(0, 0)[0] == 10, "bottom-left pixel");
            check(img.px(2, 2)[0] == 20, "top-right pixel");
        }
        
        // Test 2: blend math accuracy
        {
            uint8_t base[3] = {100, 150, 200};
            uint8_t over[3] = {50, 50, 50};
            uint8_t out[3];
            
            Blend::blendPixel(Blend::ADD, base, over, out);
            check(out[0] == 150 && out[1] == 200 && out[2] == 250, "add blend");
            
            Blend::blendPixel(Blend::SUBTRACT, base, over, out);
            check(out[0] == 50 && out[1] == 100 && out[2] == 150, "subtract blend");
            
            uint8_t gray[3] = {128, 128, 128};
            Blend::blendPixel(Blend::MULTIPLY, base, gray, out);
            check(out[0] == 50 && out[1] == 75 && out[2] == 100, "multiply by 50% gray");
        }
        
        // Test 3: saturation limits  
        {
            int bigAdd = 200 + 100;
            check((bigAdd > 255 ? 255 : bigAdd) == 255, "add saturates at 255");
            int negSub = 50 - 100;
            check((negSub < 0 ? 0 : negSub) == 0, "subtract floors at 0");
        }
        
        // Test 4: Real file test with known values
        {
            uint8_t pixels[12] = {
                255, 0, 0,
                0, 255, 0,
                0, 0, 255,
                128, 128, 128
            };
            Image test;
            test.width = 2;
            test.height = 2;
            test.pixels.assign(pixels, pixels + 12);
            TGA::save(test, "test_2x2.tga");
            
            Image loaded = TGA::load("test_2x2.tga");
            Tests::check(loaded.px(0,0)[2] == 255, "red channel at (0,0)");
            Tests::check(loaded.px(1,1)[0] == 128 && loaded.px(1,1)[1] == 128, "gray at (1,1)");
            
            uint8_t gray50[12] = {128,128,128, 128,128,128, 128,128,128, 128,128,128};
            Image grayImg;
            grayImg.width = 2;
            grayImg.height = 2;
            grayImg.pixels.assign(gray50, gray50 + 12);
            
            Image mult = Blend::apply(loaded, grayImg, Blend::MULTIPLY);
            check(mult.px(0,0)[2] == 128 && mult.px(0,0)[1] == 0, "multiply red by 50%");
            check(mult.px(1,1)[0] == 64, "multiply gray by 50%");
        }
        
        std::cout << "All tests passed\n";
    }
}

void usage(const char* prog) {
    std::cerr << "Usage:\n"
              << "  " << prog << " test\n"
              << "  " << prog << " <mode> <base> <overlay> <output>\n\n"
              << "Modes: add, subtract, multiply, screen, overlay\n";
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            usage(argv[0]);
            return 1;
        }
        
        std::string cmd = argv[1];
        
        if (cmd == "test") {
            Tests::runAll();
            return 0;
        }
        
        if (argc != 5) {
            usage(argv[0]);
            return 1;
        }
        
        Blend::Mode mode;
        if      (cmd == "add")      mode = Blend::ADD;
        else if (cmd == "subtract") mode = Blend::SUBTRACT;
        else if (cmd == "multiply") mode = Blend::MULTIPLY;
        else if (cmd == "screen")   mode = Blend::SCREEN;
        else if (cmd == "overlay")  mode = Blend::OVERLAY;
        else {
            std::cerr << "Unknown blend mode: " << cmd << "\n";
            return 1;
        }
        
        std::cout << "Loading base: "    << argv[2] << "\n";
        Image base = TGA::load(argv[2]);
        
        std::cout << "Loading overlay: " << argv[3] << "\n";
        Image overlay = TGA::load(argv[3]);
        
        std::cout << "Blending with mode: " << cmd << "\n";
        Image result = Blend::apply(base, overlay, mode);
        
        std::cout << "Saving to: " << argv[4] << "\n";
        TGA::save(result, argv[4]);
        
        std::cout << "Done!\n";
        
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

