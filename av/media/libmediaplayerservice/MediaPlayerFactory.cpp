/*
**
** Copyright 2012, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/
//#define LOG_NDEBUG 0
#define LOG_TAG "MediaPlayerFactory"
#include <utils/Log.h>

#include <cutils/properties.h>
#include <media/IMediaPlayer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <utils/Errors.h>
#include <utils/misc.h>

#include "MediaPlayerFactory.h"

#include "MidiFile.h"
#include "TestPlayerStub.h"
#include "StagefrightPlayer.h"
#include "nuplayer/NuPlayerDriver.h"

#include "CedarPlayer.h"
#include "CedarAPlayerWrapper.h"
#include "SimpleMediaFormatProbe.h"

#include <media/stagefright/foundation/hexdump.h>

#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>



namespace android {

Mutex MediaPlayerFactory::sLock;
MediaPlayerFactory::tFactoryMap MediaPlayerFactory::sFactoryMap;
bool MediaPlayerFactory::sInitComplete = false;

// TODO: Temp hack until we can register players
typedef struct {
    const char *extension;
    const player_type playertype;
} extmap;

extmap FILE_EXTS [] =  {
		{".ogg",  STAGEFRIGHT_PLAYER},
		{".wav",  STAGEFRIGHT_PLAYER},
		{".amr",  STAGEFRIGHT_PLAYER},
		{".flac", STAGEFRIGHT_PLAYER},
		{".m4a",  STAGEFRIGHT_PLAYER},
		{".m4r",  STAGEFRIGHT_PLAYER},
		{".out",  STAGEFRIGHT_PLAYER},
		{".aac",  STAGEFRIGHT_PLAYER},
		//{".3gp",  STAGEFRIGHT_PLAYER},
        //{".aac",  STAGEFRIGHT_PLAYER},
            
        {".mid",  SONIVOX_PLAYER},
        {".midi", SONIVOX_PLAYER},
        {".smf",  SONIVOX_PLAYER},
        {".xmf",  SONIVOX_PLAYER},
        {".mxmf", SONIVOX_PLAYER},
        {".imy",  SONIVOX_PLAYER},
        {".rtttl",SONIVOX_PLAYER},
        {".rtx",  SONIVOX_PLAYER},
        {".ota",  SONIVOX_PLAYER},

		{".mp3", CEDARA_PLAYER},
        {".ape", CEDARA_PLAYER},
        {".ac3", CEDARA_PLAYER},
        {".dts", CEDARA_PLAYER},
        {".wma", CEDARA_PLAYER},      
        {".mp2", CEDARA_PLAYER},
        {".mp1", CEDARA_PLAYER},
};

extmap MP4A_FILE_EXTS [] =  {
	{".m4a", CEDARX_PLAYER},
	{".m4r", CEDARX_PLAYER},
	{".3gpp", CEDARX_PLAYER},
};

extern int MovAudioOnlyDetect0(const char *url);
extern int MovAudioOnlyDetect1(int fd, int64_t offset, int64_t length);

player_type getPlayerType_l(int fd, int64_t offset, int64_t length, bool check_cedar)
{
	int r_size;
	int file_format;
    char buf[4096];
    lseek(fd, offset, SEEK_SET);
    r_size = read(fd, buf, sizeof(buf));
    lseek(fd, offset, SEEK_SET);

    long ident = *((long*)buf);

    // Ogg vorbis?
    if (ident == 0x5367674f) // 'OggS'
        return STAGEFRIGHT_PLAYER;

    // Some kind of MIDI?
    EAS_DATA_HANDLE easdata;
    if (EAS_Init(&easdata) == EAS_SUCCESS) {
        EAS_FILE locator;
        locator.path = NULL;
        locator.fd = fd;
        locator.offset = offset;
        locator.length = length;
        EAS_HANDLE  eashandle;
        if (EAS_OpenFile(easdata, &locator, &eashandle) == EAS_SUCCESS) {
            EAS_CloseFile(easdata, eashandle);
            EAS_Shutdown(easdata);
            return SONIVOX_PLAYER;
        }
        EAS_Shutdown(easdata);
    }

    if (check_cedar) {
		file_format = fd_audio_format_detect((unsigned char*)buf, r_size);
		ALOGV("getPlayerType: %d",file_format);

		if (file_format == MEDIA_FORMAT_3GP) {
			int audio_only;
			audio_only = MovAudioOnlyDetect1(fd, offset, length);
			lseek(fd, offset, SEEK_SET);

			return audio_only ? STAGEFRIGHT_PLAYER : CEDARX_PLAYER;
		}
		else
		{
			if(file_format < MEDIA_FORMAT_STAGEFRIGHT_MAX && file_format > MEDIA_FORMAT_STAGEFRIGHT_MIN){
				if (file_format == MEDIA_FORMAT_WAV || file_format == MEDIA_FORMAT_FLAC){
					ALOGV("use CEDARA_PLAYER");
					return CEDARA_PLAYER;
				}
				ALOGV("use STAGEFRIGHT_PLAYER");
				return STAGEFRIGHT_PLAYER;
			}
			else if(file_format < MEDIA_FORMAT_CEDARA_MAX && file_format > MEDIA_FORMAT_CEDARA_MIN){
				ALOGV("use CEDARA_PLAYER");
				return CEDARA_PLAYER;
			}
			else if(file_format < MEDIA_FORMAT_CEDARX_MAX && file_format > MEDIA_FORMAT_CEDARX_MIN){
				ALOGV("use CEDARX_PLAYER");
				return CEDARX_PLAYER;
			}
		}
    }

    return STAGEFRIGHT_PLAYER; 
}

player_type getPlayerType_l(const char* url)
{
	char *strpos;

	int r_size = 0;;
	int file_format = MEDIA_FORMAT_UNKOWN;
	#define datasize 4096
	unsigned char data[4096];
	FILE *fp = NULL;

	ALOGV("url %s",  url);

	if (strstr(url, "://") == NULL) {
		fp = fopen(url, "rb");
		if (fp) {
			r_size = fread(data, 1, datasize, fp);

			file_format = audio_format_detect((unsigned char*)data, r_size);
			ALOGV("getPlayerType: %d",file_format);

			fclose(fp);
			if(file_format < MEDIA_FORMAT_STAGEFRIGHT_MAX && file_format > MEDIA_FORMAT_STAGEFRIGHT_MIN){
				ALOGV("use STAGEFRIGHT_PLAYER");
				return STAGEFRIGHT_PLAYER;
			}
			else if(file_format < MEDIA_FORMAT_CEDARA_MAX && file_format > MEDIA_FORMAT_CEDARA_MIN){
				ALOGV("use CEDARA_PLAYER");
				return CEDARA_PLAYER;
			}
			else if(file_format < MEDIA_FORMAT_CEDARX_MAX && file_format > MEDIA_FORMAT_CEDARX_MIN){
				ALOGV("use CEDARX_PLAYER");
				return CEDARX_PLAYER;
			}
			else if(file_format < MEDIA_FORMAT_SONIVOX_MAX && file_format > MEDIA_FORMAT_SONIVOX_MIN){
				ALOGV("use CEDARX_PLAYER");
				return SONIVOX_PLAYER;
			}
		}
	}	

    if (TestPlayerStub::canBeUsed(url)) {
            return TEST_PLAYER;
	}

    if (!strncasecmp("http://", url, 7) || !strncasecmp("https://", url, 8)) {
		if((strpos = strrchr(url,'?')) != NULL) {
			for (int i = 0; i < NELEM(FILE_EXTS); ++i) {	
				int len = strlen(FILE_EXTS[i].extension);
					if (!strncasecmp(strpos -len, FILE_EXTS[i].extension, len)) {
						if(i==2)//net wav
						{
							return CEDARA_PLAYER;
						}
						else
						{
							return FILE_EXTS[i].playertype;
						}
					}
			}
		}
	}

    if (!strncmp("data:;base64", url, strlen("data:;base64"))) {
        return STAGEFRIGHT_PLAYER;
    }

    // use MidiFile for MIDI extensions
    int lenURL = strlen(url);
    int len;
    int start;
    for (int i = 0; i < NELEM(FILE_EXTS); ++i) {
        len = strlen(FILE_EXTS[i].extension);
        start = lenURL - len;
        if (start > 0) {
            if (!strncasecmp(url + start, FILE_EXTS[i].extension, len)) {		
            	return FILE_EXTS[i].playertype;
            }
        }
    }

    //MP4 AUDIO ONLY DETECT
    if (strstr(url, "://") == NULL) {
		for (int i = 0; i < NELEM(MP4A_FILE_EXTS); ++i) {
			len = strlen(MP4A_FILE_EXTS[i].extension);
			start = lenURL - len;
			if (start > 0) {
				if (!strncasecmp(url + start, MP4A_FILE_EXTS[i].extension, len)) {
					if (MovAudioOnlyDetect0(url))
						return STAGEFRIGHT_PLAYER;
				}
			}
		}
    }
    
    return CEDARX_PLAYER;
}


status_t MediaPlayerFactory::registerFactory_l(IFactory* factory,
                                               player_type type) {
    if (NULL == factory) {
        ALOGE("Failed to register MediaPlayerFactory of type %d, factory is"
              " NULL.", type);
        return BAD_VALUE;
    }

    if (sFactoryMap.indexOfKey(type) >= 0) {
        ALOGE("Failed to register MediaPlayerFactory of type %d, type is"
              " already registered.", type);
        return ALREADY_EXISTS;
    }

    if (sFactoryMap.add(type, factory) < 0) {
        ALOGE("Failed to register MediaPlayerFactory of type %d, failed to add"
              " to map.", type);
        return UNKNOWN_ERROR;
    }

    return OK;
}

player_type MediaPlayerFactory::getDefaultPlayerType() {
    char value[PROPERTY_VALUE_MAX];
    if (property_get("media.stagefright.use-nuplayer", value, NULL)
            && (!strcmp("1", value) || !strcasecmp("true", value))) {
        return NU_PLAYER;
    }
    
#if 0
    return STAGEFRIGHT_PLAYER;
#else
    return CEDARX_PLAYER;
#endif

}

status_t MediaPlayerFactory::registerFactory(IFactory* factory,
                                             player_type type) {
    Mutex::Autolock lock_(&sLock);
    return registerFactory_l(factory, type);
}

void MediaPlayerFactory::unregisterFactory(player_type type) {
    Mutex::Autolock lock_(&sLock);
    sFactoryMap.removeItem(type);
}

#define GET_PLAYER_TYPE_IMPL(a...)                      \
    Mutex::Autolock lock_(&sLock);                      \
                                                        \
    player_type ret = STAGEFRIGHT_PLAYER;               \
    float bestScore = 0.0;                              \
                                                        \
    for (size_t i = 0; i < sFactoryMap.size(); ++i) {   \
                                                        \
        IFactory* v = sFactoryMap.valueAt(i);           \
        float thisScore;                                \
        CHECK(v != NULL);                               \
        thisScore = v->scoreFactory(a, bestScore);      \
        if (thisScore > bestScore) {                    \
            ret = sFactoryMap.keyAt(i);                 \
            bestScore = thisScore;                      \
        }                                               \
    }                                                   \
                                                        \
    if (0.0 == bestScore) {                             \
        bestScore = getDefaultPlayerType();             \
    }                                                   \
                                                        \
    return ret;

player_type MediaPlayerFactory::getPlayerType(const sp<IMediaPlayer>& client,
                                              const char* url) {
    ALOGV("MediaPlayerFactory::getPlayerType: url = %s", url);
    
    return android::getPlayerType_l(url);
    
    #if 0
    GET_PLAYER_TYPE_IMPL(client, url);
    #endif
}

player_type MediaPlayerFactory::getPlayerType(const sp<IMediaPlayer>& client,
                                              int fd,
                                              int64_t offset,
                                              int64_t length,
                                              bool check_cedar) {
    ALOGV("MediaPlayerFactory::getPlayerType: fd = 0x%x", fd);
    
    if (true == check_cedar)
    {
        return android::getPlayerType_l(fd, offset, length, check_cedar);
    }
    else
    {
        GET_PLAYER_TYPE_IMPL(client, fd, offset, length);
    }
}

player_type MediaPlayerFactory::getPlayerType(const sp<IMediaPlayer>& client,
                                              const sp<IStreamSource> &source) {
    GET_PLAYER_TYPE_IMPL(client, source);
}

#undef GET_PLAYER_TYPE_IMPL

sp<MediaPlayerBase> MediaPlayerFactory::createPlayer(
        player_type playerType,
        void* cookie,
        notify_callback_f notifyFunc) {
    sp<MediaPlayerBase> p;
    IFactory* factory;
    status_t init_result;
    Mutex::Autolock lock_(&sLock);

    if (sFactoryMap.indexOfKey(playerType) < 0) {
        ALOGE("Failed to create player object of type %d, no registered"
              " factory", playerType);
        return p;
    }

    factory = sFactoryMap.valueFor(playerType);
    CHECK(NULL != factory);
    p = factory->createPlayer();

    if (p == NULL) {
        ALOGE("Failed to create player object of type %d, create failed",
               playerType);
        return p;
    }

    init_result = p->initCheck();
    if (init_result == NO_ERROR) {
        p->setNotifyCallback(cookie, notifyFunc);
    } else {
        ALOGE("Failed to create player object of type %d, initCheck failed"
              " (res = %d)", playerType, init_result);
        p.clear();
    }

    return p;
}

/*****************************************************************************
 *                                                                           *
 *                     Built-In Factory Implementations                      *
 *                                                                           *
 *****************************************************************************/

class StagefrightPlayerFactory :
    public MediaPlayerFactory::IFactory {
  public:
    virtual float scoreFactory(const sp<IMediaPlayer>& client,
                               int fd,
                               int64_t offset,
                               int64_t length,
                               float curScore) {
        char buf[20];
        lseek(fd, offset, SEEK_SET);
        read(fd, buf, sizeof(buf));
        lseek(fd, offset, SEEK_SET);

        long ident = *((long*)buf);

        // Ogg vorbis?
        if (ident == 0x5367674f) // 'OggS'
            return 1.0;

        return 0.0;
    }

    virtual sp<MediaPlayerBase> createPlayer() {
        ALOGV(" create StagefrightPlayer");
        return new StagefrightPlayer();
    }
};

class NuPlayerFactory : public MediaPlayerFactory::IFactory {
  public:
    virtual float scoreFactory(const sp<IMediaPlayer>& client,
                               const char* url,
                               float curScore) {
        static const float kOurScore = 0.8;

        if (kOurScore <= curScore)
            return 0.0;

        if (!strncasecmp("http://", url, 7)
                || !strncasecmp("https://", url, 8)) {
            size_t len = strlen(url);
            if (len >= 5 && !strcasecmp(".m3u8", &url[len - 5])) {
                return kOurScore;
            }

            if (strstr(url,"m3u8")) {
                return kOurScore;
            }
        }

        if (!strncasecmp("rtsp://", url, 7)) {
            return kOurScore;
        }

        return 0.0;
    }

    virtual float scoreFactory(const sp<IMediaPlayer>& client,
                               const sp<IStreamSource> &source,
                               float curScore) {
        return 1.0;
    }

    virtual sp<MediaPlayerBase> createPlayer() {
        ALOGV(" create NuPlayer");
        return new NuPlayerDriver;
    }
};

class SonivoxPlayerFactory : public MediaPlayerFactory::IFactory {
  public:
    virtual float scoreFactory(const sp<IMediaPlayer>& client,
                               const char* url,
                               float curScore) {
        static const float kOurScore = 0.4;
        static const char* const FILE_EXTS[] = { ".mid",
                                                 ".midi",
                                                 ".smf",
                                                 ".xmf",
                                                 ".mxmf",
                                                 ".imy",
                                                 ".rtttl",
                                                 ".rtx",
                                                 ".ota" };
        if (kOurScore <= curScore)
            return 0.0;

        // use MidiFile for MIDI extensions
        int lenURL = strlen(url);
        for (int i = 0; i < NELEM(FILE_EXTS); ++i) {
            int len = strlen(FILE_EXTS[i]);
            int start = lenURL - len;
            if (start > 0) {
                if (!strncasecmp(url + start, FILE_EXTS[i], len)) {
                    return kOurScore;
                }
            }
        }

        return 0.0;
    }

    virtual float scoreFactory(const sp<IMediaPlayer>& client,
                               int fd,
                               int64_t offset,
                               int64_t length,
                               float curScore) {
        static const float kOurScore = 0.8;

        if (kOurScore <= curScore)
            return 0.0;

        // Some kind of MIDI?
        EAS_DATA_HANDLE easdata;
        if (EAS_Init(&easdata) == EAS_SUCCESS) {
            EAS_FILE locator;
            locator.path = NULL;
            locator.fd = fd;
            locator.offset = offset;
            locator.length = length;
            EAS_HANDLE  eashandle;
            if (EAS_OpenFile(easdata, &locator, &eashandle) == EAS_SUCCESS) {
                EAS_CloseFile(easdata, eashandle);
                EAS_Shutdown(easdata);
                return kOurScore;
            }
            EAS_Shutdown(easdata);
        }

        return 0.0;
    }

    virtual sp<MediaPlayerBase> createPlayer() {
        ALOGV(" create MidiFile");
        return new MidiFile();
    }
};

class TestPlayerFactory : public MediaPlayerFactory::IFactory {
  public:
    virtual float scoreFactory(const sp<IMediaPlayer>& client,
                               const char* url,
                               float curScore) {
        if (TestPlayerStub::canBeUsed(url)) {
            return 1.0;
        }

        return 0.0;
    }

    virtual sp<MediaPlayerBase> createPlayer() {
        ALOGV("Create Test Player stub");
        return new TestPlayerStub();
    }
};

class CedarXPlayerFactory : public MediaPlayerFactory::IFactory {
  public:
    virtual float scoreFactory(const sp<IMediaPlayer>& client,
                               int fd,
                               int64_t offset,
                               int64_t length,
                               float curScore) {

        return 0.0;
    }

    virtual sp<MediaPlayerBase> createPlayer() {
        ALOGV(" create CedarXPlayer");
        return new CedarPlayer();
    }
};

class CedarAPlayerFactory : public MediaPlayerFactory::IFactory {
  public:
    virtual float scoreFactory(const sp<IMediaPlayer>& client,
                               int fd,
                               int64_t offset,
                               int64_t length,
                               float curScore) {

        
        return 0.0;
    }

    virtual sp<MediaPlayerBase> createPlayer() {
        ALOGV(" create CedarAPlayer");
        return new CedarAPlayerWrapper();
    }
};

void MediaPlayerFactory::registerBuiltinFactories() {
    Mutex::Autolock lock_(&sLock);

    if (sInitComplete)
        return;

    registerFactory_l(new CedarXPlayerFactory(), CEDARX_PLAYER);
    registerFactory_l(new CedarAPlayerFactory(), CEDARA_PLAYER);
    registerFactory_l(new StagefrightPlayerFactory(), STAGEFRIGHT_PLAYER);
    registerFactory_l(new NuPlayerFactory(), NU_PLAYER);
    registerFactory_l(new SonivoxPlayerFactory(), SONIVOX_PLAYER);
    registerFactory_l(new TestPlayerFactory(), TEST_PLAYER);

    sInitComplete = true;
}

}  // namespace android
