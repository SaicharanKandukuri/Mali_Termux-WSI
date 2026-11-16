#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// --- Definiții standard DDS ---
#define FOURCC_DXT1 0x31545844 // "DXT1"
#define FOURCC_DXT3 0x33545844 // "DXT3"
#define FOURCC_DXT5 0x35545844 // "DXT5"

#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define GL_RGBA8 0x8058 // Formatul nativ pe care îl înțelege Mali

#pragma pack(push, 1)
typedef struct {
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipMapCount;
    uint32_t reserved1[11];
    struct {
        uint32_t size;
        uint32_t flags;
        uint32_t fourCC;
        uint32_t rgbBitCount;
        uint32_t rBitMask;
        uint32_t gBitMask;
        uint32_t bBitMask;
        uint32_t aBitMask;
    } ddspf;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
} DDS_HEADER;
#pragma pack(pop)

typedef struct {
    int width, height;
    int components;
    int format;
    unsigned char *pixels;
} gliGenericImage;

gliGenericImage * ReadDDSFile(const char *filename, int *bufsize, int *numMipmaps) 
{
    DDS_HEADER ddsd;
    char filecode[4];
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
        printf("[Eroare] Nu pot deschide fisierul %s\n", filename);
        return NULL;
    }
    
    fread(filecode, 1, 4, fp);
    if (strncmp(filecode, "DDS ", 4) != 0) {
        printf("[Eroare] Nu este un fisier DDS valid!\n");
        fclose(fp);
        return NULL;
    }
    
    fread(&ddsd, sizeof(DDS_HEADER), 1, fp);
    
    gliGenericImage *genericImage = (gliGenericImage*)calloc(1, sizeof(gliGenericImage));
    
    *bufsize = ddsd.mipMapCount > 1 ? ddsd.pitchOrLinearSize * 2 : ddsd.pitchOrLinearSize;
    genericImage->pixels = (unsigned char*)malloc(*bufsize * sizeof(unsigned char));
    fread(genericImage->pixels, 1, *bufsize, fp);
    fclose(fp);
    
    genericImage->width = ddsd.width;
    genericImage->height = ddsd.height;
    genericImage->components = (ddsd.ddspf.fourCC == FOURCC_DXT1) ? 3 : 4;
    *numMipmaps = ddsd.mipMapCount > 0 ? ddsd.mipMapCount : 1;
    
    switch(ddsd.ddspf.fourCC) {
        case FOURCC_DXT1: genericImage->format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; break;
        case FOURCC_DXT3: genericImage->format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; break;
        case FOURCC_DXT5: genericImage->format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; break;
        default:
            printf("[Eroare] Format DXT necunoscut: 0x%X\n", ddsd.ddspf.fourCC);
            free(genericImage->pixels);
            free(genericImage);
            return NULL;
    }
    
    return genericImage;
}

// --- Functie pentru a genera un DDS de test (4x4 pixeli, DXT1) ---
void create_dummy_dds(const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) return;
    
    DDS_HEADER header;
    memset(&header, 0, sizeof(header));
    
    fwrite("DDS ", 1, 4, fp);
    
    header.size = 124;
    header.flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000; // CAPS | HEIGHT | WIDTH | PIXELFORMAT | LINEARSIZE
    header.height = 4;
    header.width = 4;
    header.pitchOrLinearSize = 8; // 4x4 DXT1 = 8 bytes
    header.mipMapCount = 1;
    
    header.ddspf.size = 32;
    header.ddspf.flags = 0x4; // DDPF_FOURCC
    header.ddspf.fourCC = FOURCC_DXT1;
    
    header.caps = 0x1000; // DDSCAPS_TEXTURE
    
    fwrite(&header, sizeof(DDS_HEADER), 1, fp);
    
    // Date DXT1 dummy (8 bytes)
    unsigned char dxt_data[8] = {0x00, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // Un pixel rosu
    fwrite(dxt_data, 1, 8, fp);
    
    fclose(fp);
    printf("[Info] Am creat fisierul de test: %s\n", filename);
}

int main() {
    const char *filename = "test_texture.dds";
    int bufsize = 0, numMipmaps = 0;

    printf("=== Test Loader DDS si Traducere pentru Mali ===\n\n");

    // 1. Generam un fisier DDS pentru test
    create_dummy_dds(filename);

    // 2. Il citim cu loader-ul nostru
    gliGenericImage *img = ReadDDSFile(filename, &bufsize, &numMipmaps);
    if (!img) {
        printf("[FAIL] Loader-ul a returnat NULL.\n");
        return 1;
    }

    // 3. Afișăm ce a citit loader-ul
    printf("\n[Rezultat Loader DDS]\n");
    printf("  Dimensiune:    %dx%d\n", img->width, img->height);
    printf("  Bufsize:       %d bytes\n", bufsize);
    printf("  Mipmaps:       %d\n", numMipmaps);
    printf("  Components:    %d\n", img->components);
    
    // 4. Simulam traducerea din shim-ul tau (gl_es_emul.c)
    printf("\n[Traducere pentru Mali-G52]\n");
    if (img->format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) {
        printf("  Aplicatia a cerut:   0x%X (GL_COMPRESSED_RGBA_S3TC_DXT1_EXT)\n", img->format);
        printf("  Mali nu intelege DXT1 nativ in GLES (conform testului anterior).\n");
        printf("  Shim-ul tau (bcdec) va decoda %d bytes DXT1 in %d bytes RGBA8.\n", 
                bufsize, img->width * img->height * 4);
        printf("  Driverul Mali va primi: 0x%X (GL_RGBA8) cu date decomprimate.\n", GL_RGBA8);
    } else {
        printf("  Format: 0x%X (Neasteptat)\n", img->format);
    }

    // 5. Curatenie
    free(img->pixels);
    free(img);
    remove(filename); // Stergem fisierul de test

    printf("\n=== Test complet (RESULT OK) ===\n");
    return 0;
}