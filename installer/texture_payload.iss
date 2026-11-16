; Hi-res texture pack 1.0.0 (installer\Output\texture-addon-1.0.0\texture-payload.json).
; One 7z (LZMA2) archive, published as a GitHub release asset. Every value is pinned:
; the download is used only when its size and SHA-256 match, and an installed pack only
; counts as current when its manifest.txt hash and every listed file size match.
; Override any define on the ISCC command line for isolated tests.
#ifndef TextureVersion
#define TextureVersion "1.0.0"
#endif
#ifndef PackGuid
#define PackGuid "5E2C8B1A-7D43-4F96-A0B5-3C19E6D2F874"
#endif
#ifndef TextureExtractedBytes
#define TextureExtractedBytes 1896227716
#endif
#ifndef TextureManifestSHA256
#define TextureManifestSHA256 "367ab2e77a73b2a876ec5d4bae43b5470fcba22f1e6181eaf44aa728a452a2f5"
#endif
#ifndef TextureFileCount
#define TextureFileCount 2905
#endif
#ifndef TextureCacheRoot
#define TextureCacheRoot "{localappdata}\RedCard 2003\Downloads"
#endif
#ifndef TextureURL
#define TextureURL "https://github.com/dalves408/redcard2003recomp/releases/download/textures-1.0.0/texture.bin"
#endif
#ifndef TextureSHA256
#define TextureSHA256 "89badf42de853aeb7cf8fa9bd7f65f5b7bbf8f3a7adaf57bc39088005d571448"
#endif
#ifndef TextureBytes
#define TextureBytes 1030982957
#endif
