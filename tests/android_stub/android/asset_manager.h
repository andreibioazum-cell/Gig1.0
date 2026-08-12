/* Минимальные объявления Android, чтобы собрать игру на Linux для тестов. */
#ifndef DS_STUB_ASSET_MANAGER_H
#define DS_STUB_ASSET_MANAGER_H

#include <stddef.h>
#include <sys/types.h>

typedef struct AAssetManager AAssetManager;
typedef struct AAsset AAsset;

#define AASSET_MODE_BUFFER 3

AAsset *AAssetManager_open(AAssetManager *mgr, const char *name, int mode);
off_t AAsset_getLength(AAsset *asset);
int AAsset_read(AAsset *asset, void *buf, size_t count);
void AAsset_close(AAsset *asset);

#endif
