/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "StagefrightMediaScanner"
#include <utils/Log.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <media/stagefright/StagefrightMediaScanner.h>

#include <media/mediametadataretriever.h>
#include <private/media/VideoFrame.h>

// Sonivox includes
#include <libsonivox/eas.h>

namespace android {

StagefrightMediaScanner::StagefrightMediaScanner() {}

StagefrightMediaScanner::~StagefrightMediaScanner() {}

static int FileHasAcceptableExtension(const char *extension) {
    static const char *kValidExtensions[] = {
        ".mp3", ".mp4", ".m4a", ".3gp", ".3gpp", ".3g2", ".3gpp2",
        ".mpeg", ".ogg", ".mid", ".smf", ".imy", ".wma", ".aac",
        ".wav", ".amr", ".midi", ".xmf", ".rtttl", ".rtx", ".ota",
        ".mka", ".fl", ".flac", ".mxmf",
        ".mpeg", ".mpg"
    };

    static const char *kValidExtensionsAW[] = {
       ".mkv", ".rmvb", ".rm", ".mov", ".flv", ".f4v", ".avi",
       ".mp1", ".mp2", ".awb", ".oga", ".ape", ".ac3",
       ".dts", ".omg", ".oma", ".midi", ".m4v", ".wmv", ".asf",
       ".vob", ".pmp", ".m4r", ".ra", ".webm",
       ".ts",".m2ts"
    };

    size_t kNumValidExtensions;
    
    kNumValidExtensions =
        sizeof(kValidExtensions) / sizeof(kValidExtensions[0]);

    for (size_t i = 0; i < kNumValidExtensions; ++i) {
        if (!strcasecmp(extension, kValidExtensions[i])) {
            return 1;
        }
    }

    kNumValidExtensions =
            sizeof(kValidExtensionsAW) / sizeof(kValidExtensionsAW[0]);

    for (size_t i = 0; i < kNumValidExtensions; ++i) {
		if (!strcasecmp(extension, kValidExtensionsAW[i])) {
			return 2;
		}
	}

    return 0;
}

static MediaScanResult HandleMIDI(
        const char *filename, MediaScannerClient *client) {
    // get the library configuration and do sanity check
    const S_EAS_LIB_CONFIG* pLibConfig = EAS_Config();
    if ((pLibConfig == NULL) || (LIB_VERSION != pLibConfig->libVersion)) {
        ALOGE("EAS library/header mismatch\n");
        return MEDIA_SCAN_RESULT_ERROR;
    }
    EAS_I32 temp;

    // spin up a new EAS engine
    EAS_DATA_HANDLE easData = NULL;
    EAS_HANDLE easHandle = NULL;
    EAS_RESULT result = EAS_Init(&easData);
    if (result == EAS_SUCCESS) {
        EAS_FILE file;
        file.path = filename;
        file.fd = 0;
        file.offset = 0;
        file.length = 0;
        result = EAS_OpenFile(easData, &file, &easHandle);
    }
    if (result == EAS_SUCCESS) {
        result = EAS_Prepare(easData, easHandle);
    }
    if (result == EAS_SUCCESS) {
        result = EAS_ParseMetaData(easData, easHandle, &temp);
    }
    if (easHandle) {
        EAS_CloseFile(easData, easHandle);
    }
    if (easData) {
        EAS_Shutdown(easData);
    }

    if (result != EAS_SUCCESS) {
        return MEDIA_SCAN_RESULT_SKIPPED;
    }

    char buffer[20];
    sprintf(buffer, "%ld", temp);
    status_t status = client->addStringTag("duration", buffer);
    if (status != OK) {
        return MEDIA_SCAN_RESULT_ERROR;
    }
    return MEDIA_SCAN_RESULT_OK;
}

MediaScanResult StagefrightMediaScanner::processFile(
        const char *path, const char *mimeType,
        MediaScannerClient &client) {
    ALOGV("processFile '%s'.", path);

    client.setLocale(locale());
    client.beginFile();
    MediaScanResult result = processFileInternal(path, mimeType, client);
    client.endFile();
    return result;
}

MediaScanResult StagefrightMediaScanner::processFileInternal(
        const char *path, const char *mimeType,
        MediaScannerClient &client) {
    const char *extension = strrchr(path, '.');
    int faccext_ret;
    
    if (!extension) {
        return MEDIA_SCAN_RESULT_SKIPPED;
    }

    faccext_ret = FileHasAcceptableExtension(extension);
    if (!faccext_ret) {
        return MEDIA_SCAN_RESULT_SKIPPED;
    }

    if (!strcasecmp(extension, ".mid")
            || !strcasecmp(extension, ".smf")
            || !strcasecmp(extension, ".imy")
            || !strcasecmp(extension, ".midi")
            || !strcasecmp(extension, ".xmf")
            || !strcasecmp(extension, ".rtttl")
            || !strcasecmp(extension, ".rtx")
            || !strcasecmp(extension, ".ota")
            || !strcasecmp(extension, ".mxmf")) {
        return HandleMIDI(path, &client);
    }

    sp<MediaMetadataRetriever> mRetriever(new MediaMetadataRetriever);


    /* Disable following code if we implement interface setting
     * data source with file descriptor
     */
     /*
    status_t status;
    status = mRetriever->setDataSource(path);
    */

    /* Enable following code if we implement interface setting
     * data source with file descriptor
     */

    
    int fd = open(path, O_RDONLY | O_LARGEFILE);
    status_t status;
    if (fd < 0) {
        // couldn't open it locally, maybe the media server can?
        status = mRetriever->setDataSource(path);
    } else {
        status = mRetriever->setDataSource(fd, 0, 0x7ffffffffffffffL);
        close(fd);
    }
    

    if (status) {
        return MEDIA_SCAN_RESULT_ERROR;
    }

    const char *value;
    if ((value = mRetriever->extractMetadata(
                    METADATA_KEY_MIMETYPE)) != NULL) {
        status = client.setMimeType(value);
        if (status) {
            return MEDIA_SCAN_RESULT_ERROR;
        }
    }

    struct KeyMap {
        const char *tag;
        int key;
    };
    static const KeyMap kKeyMap[] = {
        { "tracknumber", METADATA_KEY_CD_TRACK_NUMBER },
        { "discnumber", METADATA_KEY_DISC_NUMBER },
        { "album", METADATA_KEY_ALBUM },
        { "artist", METADATA_KEY_ARTIST },
        { "albumartist", METADATA_KEY_ALBUMARTIST },
        { "composer", METADATA_KEY_COMPOSER },
        { "genre", METADATA_KEY_GENRE },
        { "title", METADATA_KEY_TITLE },
        { "year", METADATA_KEY_YEAR },
        { "duration", METADATA_KEY_DURATION },
        { "writer", METADATA_KEY_WRITER },
        { "compilation", METADATA_KEY_COMPILATION },
        { "isdrm", METADATA_KEY_IS_DRM },
        { "width", METADATA_KEY_VIDEO_WIDTH },
        { "height", METADATA_KEY_VIDEO_HEIGHT },
    };
    static const size_t kNumEntries = sizeof(kKeyMap) / sizeof(kKeyMap[0]);

    for (size_t i = 0; i < kNumEntries; ++i) {
        const char *value;
        if ((value = mRetriever->extractMetadata(kKeyMap[i].key)) != NULL) {
            status = client.addStringTag(kKeyMap[i].tag, value);
            if (status != OK) {
                return MEDIA_SCAN_RESULT_ERROR;
            }
        }
    }

    return MEDIA_SCAN_RESULT_OK;
}

char *StagefrightMediaScanner::extractAlbumArt(int fd) {
    ALOGV("extractAlbumArt %d", fd);

    off64_t size = lseek64(fd, 0, SEEK_END);
    if (size < 0) {
        return NULL;
    }
    lseek64(fd, 0, SEEK_SET);

    sp<MediaMetadataRetriever> mRetriever(new MediaMetadataRetriever);
    if (mRetriever->setDataSource(fd, 0, size) == OK) {
        sp<IMemory> mem = mRetriever->extractAlbumArt();

        if (mem != NULL) {
            MediaAlbumArt *art = static_cast<MediaAlbumArt *>(mem->pointer());

            char *data = (char *)malloc(art->mSize + 4);
            *(int32_t *)data = art->mSize;
            memcpy(&data[4], &art[1], art->mSize);

            return data;
        }
    }

    return NULL;
}

}  // namespace android
