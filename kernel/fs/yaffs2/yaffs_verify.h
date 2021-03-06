

#ifndef __YAFFS_VERIFY_H__
#define __YAFFS_VERIFY_H__

#include "yaffs_guts.h"

void yaffs_VerifyBlock(yaffs_Device *dev, yaffs_BlockInfo *bi, int n);
void yaffs_VerifyCollectedBlock(yaffs_Device *dev, yaffs_BlockInfo *bi, int n);
void yaffs_VerifyBlocks(yaffs_Device *dev);

void yaffs_VerifyObjectHeader(yaffs_Object *obj, yaffs_ObjectHeader *oh, yaffs_ExtendedTags *tags, int parentCheck);
void yaffs_VerifyFile(yaffs_Object *obj);
void yaffs_VerifyHardLink(yaffs_Object *obj);
void yaffs_VerifySymlink(yaffs_Object *obj);
void yaffs_VerifySpecial(yaffs_Object *obj);
void yaffs_VerifyObject(yaffs_Object *obj);
void yaffs_VerifyObjects(yaffs_Device *dev);
void yaffs_VerifyObjectInDirectory(yaffs_Object *obj);
void yaffs_VerifyDirectory(yaffs_Object *directory);
void yaffs_VerifyFreeChunks(yaffs_Device *dev);

int yaffs_VerifyFileSanity(yaffs_Object *obj);

int yaffs_SkipVerification(yaffs_Device *dev);

#endif

