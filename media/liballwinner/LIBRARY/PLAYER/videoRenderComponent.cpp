
//#define CONFIG_LOG_LEVEL    OPTION_LOG_LEVEL_DETAIL
#define LOG_TAG "videoRenderComponent"
#include "log.h"

#include <pthread.h>
#include <semaphore.h>
#include <malloc.h>
#include <memory.h>
#include <time.h>

#include "videoRenderComponent.h"
#include "messageQueue.h"
#include "layerControl.h"
#include "memoryAdapter.h"
#include <deinterlace.h>

#include<sys/types.h>  
#include<sys/stat.h>
#include<fcntl.h>
#include <linux/ioctl.h>
#include <hardware/hwcomposer.h>
#include <cutils/properties.h>

#define USE_DETNTERLACE 1
#define SEND_PTS_TO_SF	1

typedef struct VideoRenderCompContext
{
    //* created at initialize time.
    MessageQueue*       mq;
    sem_t               startMsgReplySem;
    sem_t               stopMsgReplySem;
    sem_t               pauseMsgReplySem;
    sem_t               resetMsgReplySem;
    sem_t               eosMsgReplySem;
    sem_t               quitMsgReplySem;
    sem_t               set3DModeReplySem;
    sem_t               setWindowReplySem;
    sem_t               setHideVideoSem;
    sem_t               setHoldLastPictureSem;
    
    int                 nStartReply;
    int                 nStopReply;
    int                 nPauseReply;
    int                 nResetReply;
    int                 nSet3DModeReply;
    
    pthread_t           sRenderThread;
    
    enum EPLAYERSTATUS  eStatus;
    void*               pNativeWindow;
    LayerCtrl*          pLayerCtrl;
    VideoDecComp*       pDecComp;
    
    enum EPICTURE3DMODE ePicture3DMode;
    enum EDISPLAY3DMODE eDisplay3DMode;
    
    //* objects set by user.
    AvTimer*            pAvTimer;
    PlayerCallback      callback;
    void*               pUserData;
    int                 bEosFlag;
	int                 nRotationAngle;
    int                 nRequsetPictureNum;

    Deinterlace         *di;
    
    VideoPicture*       pDeinterlacePrePicture;
    int                 nGpuYAlign;
    int                 nGpuCAlign;
	int 				bSyncFirstPictureFlag;
}VideoRenderCompContext;

static void* VideoRenderThread(void* arg);
static void PostRenderMessage(MessageQueue* mq);
static int IsVideoWithTwoStream(VideoDecComp* pDecComp);
static int LayerCallback(void* pUserData, int eMessageId, void* param);
static void CheckScreenRotateAngle(VideoRenderCompContext* p,
                                   VideoPicture* pPicture,
                                   int bNeedResetAngleFlag);
static void showVideoDecodefps(VideoPicture* pPicture);

VideoRenderComp* VideoRenderCompCreate(void)
{
    VideoRenderCompContext* p;
    int                     err;
    
    p = (VideoRenderCompContext*)malloc(sizeof(VideoRenderCompContext));
    if(p == NULL)
    {
        loge("memory alloc fail.");
        return NULL;
    }
    memset(p, 0, sizeof(*p));

    p->mq = MessageQueueCreate(4, "VideoRenderMq");
    if(p->mq == NULL)
    {
        loge("video render component create message queue fail.");
        free(p);
        return NULL;
    }
    
    sem_init(&p->startMsgReplySem, 0, 0);
    sem_init(&p->stopMsgReplySem, 0, 0);
    sem_init(&p->pauseMsgReplySem, 0, 0);
    sem_init(&p->resetMsgReplySem, 0, 0);
    sem_init(&p->eosMsgReplySem, 0, 0);
    sem_init(&p->quitMsgReplySem, 0, 0);
    sem_init(&p->set3DModeReplySem, 0, 0);
    sem_init(&p->setWindowReplySem, 0, 0);
    sem_init(&p->setHideVideoSem, 0, 0);
    sem_init(&p->setHoldLastPictureSem, 0, 0);
    
    p->eStatus = PLAYER_STATUS_STOPPED;

    p->di = DeinterlaceCreate();
    if (!p->di)
    {
        logw("No deinterlace...");
    }

    err = pthread_create(&p->sRenderThread, NULL, VideoRenderThread, p);
    if(err != 0)
    {
        loge("video render component create thread fail.");
        sem_destroy(&p->startMsgReplySem);
        sem_destroy(&p->stopMsgReplySem);
        sem_destroy(&p->pauseMsgReplySem);
        sem_destroy(&p->resetMsgReplySem);
        sem_destroy(&p->eosMsgReplySem);
        sem_destroy(&p->quitMsgReplySem);
        sem_destroy(&p->set3DModeReplySem);
        sem_destroy(&p->setWindowReplySem);
        sem_destroy(&p->setHideVideoSem);
        sem_destroy(&p->setHoldLastPictureSem);
        MessageQueueDestroy(p->mq);
        if (p->di)
        {
            delete p->di;
    		p->di = NULL;
		}
        free(p);
        return NULL;
    }

#if(CONFIG_CHIP==OPTION_CHIP_1680 || CONFIG_CHIP==OPTION_CHIP_1667)  //* on 1680, Y-Align is 16, C-Align is 16, 
    p->nGpuYAlign = 16;
    p->nGpuCAlign = 16;
#elif(CONFIG_CHIP==OPTION_CHIP_1673)//* on 1673, Y-Align is 32, C-Align is 16,
    p->nGpuYAlign = 32;
    p->nGpuCAlign = 16;
#else                               //* on others, Y-Align is 16, C-Align is 8,
    p->nGpuYAlign = 16;
    p->nGpuCAlign = 8;
#endif

    
    return (VideoRenderComp*)p;
}


int VideoRenderCompDestroy(VideoRenderComp* v)
{
    void*                   status;
    VideoRenderCompContext* p;
    Message                 msg;
    
    p = (VideoRenderCompContext*)v;
    
    msg.messageId = MESSAGE_ID_QUIT;
    msg.params[0] = (uintptr_t)&p->quitMsgReplySem;
    msg.params[1] = msg.params[2] = msg.params[3] = 0;
    
    if(MessageQueuePostMessage(p->mq, &msg) != 0)
    {
        loge("fatal error, video render component post message fail.");
        abort();
    }
    
    SemTimedWait(&p->quitMsgReplySem, -1);
    pthread_join(p->sRenderThread, &status);
    
    sem_destroy(&p->startMsgReplySem);
    sem_destroy(&p->stopMsgReplySem);
    sem_destroy(&p->pauseMsgReplySem);
    sem_destroy(&p->resetMsgReplySem);
    sem_destroy(&p->eosMsgReplySem);
    sem_destroy(&p->quitMsgReplySem);
    sem_destroy(&p->set3DModeReplySem);
    sem_destroy(&p->setWindowReplySem);
    sem_destroy(&p->setHideVideoSem);
    sem_destroy(&p->setHoldLastPictureSem);

    if(p->pDeinterlacePrePicture != NULL)
        VideoDecCompReturnPicture(p->pDecComp, p->pDeinterlacePrePicture);

    if (p->di)
    {
        delete p->di;
        p->di = NULL;
    }
    
    MessageQueueDestroy(p->mq);
    free(p);
    
    return 0;   
}


int VideoRenderCompStart(VideoRenderComp* v)
{
    VideoRenderCompContext* p;
    Message                 msg;
    
    p = (VideoRenderCompContext*)v;
    
    logv("video render component starting");
    
    msg.messageId = MESSAGE_ID_START;
    msg.params[0] = (uintptr_t)&p->startMsgReplySem;
    msg.params[1] = (uintptr_t)&p->nStartReply;
    msg.params[2] = msg.params[3] = 0;
    
    if(MessageQueuePostMessage(p->mq, &msg) != 0)
    {
        loge("fatal error, video render component post message fail.");
        abort();
    }
    
    if(SemTimedWait(&p->startMsgReplySem, -1) < 0)
    {
        loge("video render component wait for start finish timeout.");
        return -1;
    }
    
    return p->nStartReply;
}


int VideoRenderCompStop(VideoRenderComp* v)
{
    VideoRenderCompContext* p;
    Message                 msg;
    
    p = (VideoRenderCompContext*)v;
    
    logv("video render component stopping");
    
    msg.messageId = MESSAGE_ID_STOP;
    msg.params[0] = (uintptr_t)&p->stopMsgReplySem;
    msg.params[1] = (uintptr_t)&p->nStopReply;
    msg.params[2] = msg.params[3] = 0;
    
    if(MessageQueuePostMessage(p->mq, &msg) != 0)
    {
        loge("fatal error, video render component post message fail.");
        abort();
    }
    
    if(SemTimedWait(&p->stopMsgReplySem, -1) < 0)
    {
        loge("video render component wait for stop finish timeout.");
        return -1;
    }
    
    return p->nStopReply;
}


int VideoRenderCompPause(VideoRenderComp* v)
{
    VideoRenderCompContext* p;
    Message                 msg;
    
    p = (VideoRenderCompContext*)v;
    
    logv("video render component pausing");
    
    msg.messageId = MESSAGE_ID_PAUSE;
    msg.params[0] = (uintptr_t)&p->pauseMsgReplySem;
    msg.params[1] = (uintptr_t)&p->nPauseReply;
    msg.params[2] = msg.params[3] = 0;
    
    if(MessageQueuePostMessage(p->mq, &msg) != 0)
    {
        loge("fatal error, video render component post message fail.");
        abort();
    }
    
    if(SemTimedWait(&p->pauseMsgReplySem, -1) < 0)
    {
        loge("video render component wait for pause finish timeout.");
        return -1;
    }
    
    return p->nPauseReply;
}


enum EPLAYERSTATUS VideoRenderCompGetStatus(VideoRenderComp* v)
{
    VideoRenderCompContext* p;
    p = (VideoRenderCompContext*)v;
    return p->eStatus;
}


int VideoRenderCompReset(VideoRenderComp* v)
{
    VideoRenderCompContext* p;
    Message                 msg;
    
    p = (VideoRenderCompContext*)v;
    
    logv("video render component reseting");
    
    msg.messageId = MESSAGE_ID_RESET;
    msg.params[0] = (uintptr_t)&p->resetMsgReplySem;
    msg.params[1] = (uintptr_t)&p->nResetReply;
    msg.params[2] = msg.params[3] = 0;
    
    if(MessageQueuePostMessage(p->mq, &msg) != 0)
    {
        loge("fatal error, video render component post message fail.");
        abort();
    }
    
    if(SemTimedWait(&p->resetMsgReplySem, -1) < 0)
    {
        loge("video render component wait for reset finish timeout.");
        return -1;
    }
    
    return p->nResetReply;
}


int VideoRenderCompSetEOS(VideoRenderComp* v)
{
    VideoRenderCompContext* p;
    Message                 msg;
    
    p = (VideoRenderCompContext*)v;
    
    logv("video render component setting EOS.");
    
    msg.messageId = MESSAGE_ID_EOS;
    msg.params[0] = (uintptr_t)&p->eosMsgReplySem;
    msg.params[1] = msg.params[2] = msg.params[3] = 0;
    
    if(MessageQueuePostMessage(p->mq, &msg) != 0)
    {
        loge("fatal error, video render component post message fail.");
        abort();
    }
    
    if(SemTimedWait(&p->eosMsgReplySem, -1) < 0)
    {
        loge("video render component wait for setting eos finish timeout.");
        return -1;
    }
    
    return 0;
}


int VideoRenderCompSetCallback(VideoRenderComp* v, PlayerCallback callback, void* pUserData)
{
    VideoRenderCompContext* p;
    
    p = (VideoRenderCompContext*)v;
    
    p->callback  = callback;
    p->pUserData = pUserData;
    
    return 0;
}


int VideoRenderCompSetTimer(VideoRenderComp* v, AvTimer* timer)
{
    VideoRenderCompContext* p;
    p = (VideoRenderCompContext*)v;
    p->pAvTimer  = timer;
    return 0;
}


int VideoRenderCompSetWindow(VideoRenderComp* v, void* pNativeWindow)
{
    VideoRenderCompContext* p;
    Message                 msg;
    
    p = (VideoRenderCompContext*)v;
    
    logv("video render component setting window.");
    
    msg.messageId = MESSAGE_ID_SET_WINDOW;
    msg.params[0] = (uintptr_t)&p->setWindowReplySem;
    msg.params[1] = 0;
    msg.params[2] = (uintptr_t)pNativeWindow;
    msg.params[3] = 0;
    
    if(MessageQueuePostMessage(p->mq, &msg) != 0)
    {
        loge("fatal error, video render component post message fail.");
        abort();
    }
    
    if(SemTimedWait(&p->setWindowReplySem, -1) < 0)
    {
        loge("video render component wait for setting window finish timeout.");
        return -1;
    }
    
    return 0;
}


int VideoRenderCompSetDecodeComp(VideoRenderComp* v, VideoDecComp* d)
{
    VideoRenderCompContext* p;
    p = (VideoRenderCompContext*)v;
    p->pDecComp  = d;
    return 0;
}


int VideoRenderSet3DMode(VideoRenderComp* v, 
                         enum EPICTURE3DMODE ePicture3DMode,
                         enum EDISPLAY3DMODE eDisplay3DMode)
{
    VideoRenderCompContext* p;
    Message                 msg;
    
    p = (VideoRenderCompContext*)v;
    
    logv("video render component setting 3d mode.");
    
    msg.messageId = MESSAGE_ID_SET_3D_MODE;
    msg.params[0] = (uintptr_t)&p->set3DModeReplySem;
    msg.params[1] = (uintptr_t)&p->nSet3DModeReply;
    msg.params[2] = (uintptr_t)ePicture3DMode;
    msg.params[3] = (uintptr_t)eDisplay3DMode;
    
    if(MessageQueuePostMessage(p->mq, &msg) != 0)
    {
        loge("fatal error, video render component post message fail.");
        abort();
    }
    
    if(SemTimedWait(&p->set3DModeReplySem, -1) < 0)
    {
        loge("video render component wait for setting 3d mode finish timeout.");
        return -1;
    }
    
    return p->nSet3DModeReply;
}


int VideoRenderGet3DMode(VideoRenderComp* v, 
                         enum EPICTURE3DMODE* ePicture3DMode,
                         enum EDISPLAY3DMODE* eDisplay3DMode)
{
    VideoRenderCompContext* p;
    p = (VideoRenderCompContext*)v;
    *ePicture3DMode = p->ePicture3DMode;
    *eDisplay3DMode = p->eDisplay3DMode;
    return 0;
}


int VideoRenderVideoHide(VideoRenderComp* v, int bHideVideo)
{
    VideoRenderCompContext* p;
    Message                 msg;
    
    p = (VideoRenderCompContext*)v;
    
    logv("video render component setting video hide(%d).", bHideVideo);
    
    msg.messageId = MESSAGE_ID_SET_VIDEO_HIDE;
    msg.params[0] = (uintptr_t)&p->setHideVideoSem;
    msg.params[1] = 0;
    msg.params[2] = bHideVideo;
    msg.params[3] = 0;
    
    if(MessageQueuePostMessage(p->mq, &msg) != 0)
    {
        loge("fatal error, video render component post message fail.");
        abort();
    }
    
    if(SemTimedWait(&p->setHideVideoSem, -1) < 0)
    {
        loge("video render component wait for setting 3d mode finish timeout.");
        return -1;
    }
    
    return 0;
}


int VideoRenderSetHoldLastPicture(VideoRenderComp* v, int bHold)
{
    VideoRenderCompContext* p;
    Message                 msg;
    
    p = (VideoRenderCompContext*)v;
    
    logv("video render component setting hold last picture(bHold=%d).", bHold);
    
    msg.messageId = MESSAGE_ID_SET_HOLD_LAST_PICTURE;
    msg.params[0] = (uintptr_t)&p->setHoldLastPictureSem;
    msg.params[1] = 0;
    msg.params[2] = bHold;
    msg.params[3] = 0;
    
    if(MessageQueuePostMessage(p->mq, &msg) != 0)
    {
        loge("fatal error, video render component post message fail.");
        abort();
    }
    
    if(SemTimedWait(&p->setHoldLastPictureSem, -1) < 0)
    {
        loge("video render component wait for setting 3d mode finish timeout.");
        return -1;
    }
    
    return 0;
}

void VideoRenderCompSetProtecedFlag(VideoRenderComp* v, int bProtectedFlag)
{
    //*TODO
    CDX_PLAYER_UNUSE(v);
	CDX_PLAYER_UNUSE(bProtectedFlag);
    return;
}

int VideoRenderCompSetSyncFirstPictureFlag(VideoRenderComp* v, int bSyncFirstPictureFlag)
{
	VideoRenderCompContext* p;
	p = (VideoRenderCompContext*)v;

	p->bSyncFirstPictureFlag = bSyncFirstPictureFlag;

	return 0;
}

static void* VideoRenderThread(void* arg)
{
    VideoRenderCompContext* p;
    Message                 msg;
    int                     ret;
    sem_t*                  pReplySem;
    int*                    pReplyValue;
    int64_t                 nCurTime;
    int                     nWaitTime;
    int                     bFirstPictureShowed;
    int                     bFirstPtsNotified;
    int                     bVideoWithTwoStream;
    VideoPicture*           pPicture;
    VideoPicture*           pSecondPictureOf3DMode;
    VideoPicture*           pLayerBuffer;
    VideoPicture*           pSecondLayerBufferOf3DMode;
    int                     bHideVideo;
    int                     bHoldLastPicture;
    int                     bResolutionChange;
    int                     bNeedResetLayerParams;
    int                     bDeinterlaceFlag;
    int                     nDeinterlaceDispNum;
    VideoPicture*           pPreLayerBuffer;
    int                     nLayerBufferMode;
    int                     bNeedResetAngleFlag;
    
    p = (VideoRenderCompContext*)arg;
    bFirstPictureShowed        = 0;
    bFirstPtsNotified          = 0;
    bVideoWithTwoStream        = 0;
    pPicture                   = NULL;
    pSecondPictureOf3DMode     = NULL;
    pLayerBuffer               = NULL;
    pSecondLayerBufferOf3DMode = NULL;
    bHideVideo                 = 0;
    bHoldLastPicture           = 0;
    bResolutionChange          = 0;
    bNeedResetLayerParams      = 0;
    bDeinterlaceFlag           = 0;
    nDeinterlaceDispNum        = 1;
    pPreLayerBuffer            = NULL;
    nLayerBufferMode           = 0;
    bNeedResetAngleFlag        = 0;
    
    while(1)
    {
        if(MessageQueueGetMessage(p->mq, &msg) < 0)
        {
            loge("get message fail.");
            continue;
        }
        
process_message:
        pReplySem   = (sem_t*)msg.params[0];
        pReplyValue = (int*)msg.params[1];
        
        if(msg.messageId == MESSAGE_ID_START)
        {
            logi("process MESSAGE_ID_START message");
            if(p->eStatus == PLAYER_STATUS_STARTED)
            {
                logw("already in started status.");
                PostRenderMessage(p->mq);
                *pReplyValue = -1;
                sem_post(pReplySem);
                continue;
            }
            
            if(p->eStatus == PLAYER_STATUS_STOPPED)
            {
                bFirstPictureShowed = 0;
                bFirstPtsNotified   = 0;
                p->bEosFlag = 0;
            }
            
            //* send a render message to start decoding.
            PostRenderMessage(p->mq);
            
            p->eStatus = PLAYER_STATUS_STARTED;
            *pReplyValue = 0;
            sem_post(pReplySem);
        }
        else if(msg.messageId == MESSAGE_ID_STOP)
        {
            logi("process MESSAGE_ID_STOP message");
            if(p->eStatus == PLAYER_STATUS_STOPPED)
            {
                logw("already in stopped status.");
                *pReplyValue = -1;
                sem_post(pReplySem);
                continue;
            }
            
            //* return buffers before stop.
            if(pLayerBuffer != NULL)
            {
                if(p->pLayerCtrl != NULL)
                {
                    if(pSecondLayerBufferOf3DMode == NULL)
                        LayerQueueBuffer(p->pLayerCtrl, pLayerBuffer, 0);
                    else
                        LayerQueue3DBuffer(p->pLayerCtrl, pLayerBuffer, pSecondLayerBufferOf3DMode, 0);
                }
                pLayerBuffer = NULL;
                pSecondLayerBufferOf3DMode = NULL;
            }
			
			if(pPreLayerBuffer != NULL)
			{
				if(p->pLayerCtrl != NULL)
				{
					if(pSecondLayerBufferOf3DMode == NULL)
						LayerQueueBuffer(p->pLayerCtrl, pPreLayerBuffer, 0);
					else
						LayerQueue3DBuffer(p->pLayerCtrl, pPreLayerBuffer, pSecondLayerBufferOf3DMode, 0);
				}
				pPreLayerBuffer = NULL;
				pSecondLayerBufferOf3DMode = NULL;
			}
            if(pPicture != NULL)
            {
                if(p->pDecComp != NULL)
                {
                    VideoDecCompReturnPicture(p->pDecComp, pPicture);
                    if(pSecondPictureOf3DMode != NULL)
                        VideoDecCompReturnPicture(p->pDecComp, pSecondPictureOf3DMode);
                }
                pPicture = NULL;
                pSecondPictureOf3DMode = NULL;
            }
            
            if(p->pLayerCtrl != NULL)
            {
                if(bHoldLastPicture)
                    LayerCtrlHoldLastPicture(p->pLayerCtrl, 1);
                else
                {
                    LayerCtrlHoldLastPicture(p->pLayerCtrl, 0);
                    if(LayerCtrlIsVideoShow(p->pLayerCtrl) == 1)
                        LayerCtrlHideVideo(p->pLayerCtrl);
                }
            }
               
            //* set status to stopped.
            p->eStatus = PLAYER_STATUS_STOPPED;
            *pReplyValue = 0;
            sem_post(pReplySem);
        }
        else if(msg.messageId == MESSAGE_ID_PAUSE)
        {
            logi("process MESSAGE_ID_PAUSE message");
            if(p->eStatus != PLAYER_STATUS_STARTED  &&
               !(p->eStatus == PLAYER_STATUS_PAUSED && bFirstPictureShowed == 0))
            {
                logw("not in started status, pause operation invalid.");
                *pReplyValue = -1;
                sem_post(pReplySem);
                continue;
            }
            
            //* set status to paused.
            p->eStatus = PLAYER_STATUS_PAUSED;
            if(bFirstPictureShowed == 0)
                PostRenderMessage(p->mq);   //* post a decode message to decode the first picture.
            *pReplyValue = 0;
            sem_post(pReplySem);
        }
        else if(msg.messageId == MESSAGE_ID_QUIT)
        {
            logi("process MESSAGE_ID_QUIT message");
            //* return buffers before quit.
            if(pLayerBuffer != NULL)
            {
                if(p->pLayerCtrl != NULL)
                {
                    if(pSecondLayerBufferOf3DMode == NULL)
                        LayerQueueBuffer(p->pLayerCtrl, pLayerBuffer, 0);
                    else
                        LayerQueue3DBuffer(p->pLayerCtrl, pLayerBuffer, pSecondLayerBufferOf3DMode, 0);
                }
                pLayerBuffer = NULL;
                pSecondLayerBufferOf3DMode = NULL;
            }
			if(pPreLayerBuffer != NULL)
			{
				if(p->pLayerCtrl != NULL)
				{
					if(pSecondLayerBufferOf3DMode == NULL)
						LayerQueueBuffer(p->pLayerCtrl, pPreLayerBuffer, 0);
					else
						LayerQueue3DBuffer(p->pLayerCtrl, pPreLayerBuffer, pSecondLayerBufferOf3DMode, 0);
				}
				pPreLayerBuffer = NULL;
				pSecondLayerBufferOf3DMode = NULL;
			}
            
            if(pPicture != NULL)
            {
                if(p->pDecComp != NULL)
                {
                    VideoDecCompReturnPicture(p->pDecComp, pPicture);
                    if(pSecondPictureOf3DMode != NULL)
                        VideoDecCompReturnPicture(p->pDecComp, pSecondPictureOf3DMode);
                }
                pPicture = NULL;
                pSecondPictureOf3DMode = NULL;
            }
            
            sem_post(pReplySem);
            p->eStatus = PLAYER_STATUS_STOPPED;
            break;
        }
        else if(msg.messageId == MESSAGE_ID_RESET)
        {
            logi("process MESSAGE_ID_RESET message");
            //* return buffers before quit.
            if(pLayerBuffer != NULL)
            {
                if(p->pLayerCtrl != NULL)
                {
                    if(pSecondLayerBufferOf3DMode == NULL)
                        LayerQueueBuffer(p->pLayerCtrl, pLayerBuffer, 0);
                    else
                        LayerQueue3DBuffer(p->pLayerCtrl, pLayerBuffer, pSecondLayerBufferOf3DMode, 0);
                }
                pLayerBuffer = NULL;
                pSecondLayerBufferOf3DMode = NULL;
            }
			if(pPreLayerBuffer != NULL)
			{
				if(p->pLayerCtrl != NULL)
				{
					if(pSecondLayerBufferOf3DMode == NULL)
						LayerQueueBuffer(p->pLayerCtrl, pPreLayerBuffer, 0);
					else
						LayerQueue3DBuffer(p->pLayerCtrl, pPreLayerBuffer, pSecondLayerBufferOf3DMode, 0);
				}
				pPreLayerBuffer = NULL;
				pSecondLayerBufferOf3DMode = NULL;
			}
            if(pPicture != NULL)
            {
                if(p->pDecComp != NULL)
                {
                    VideoDecCompReturnPicture(p->pDecComp, pPicture);
                    if(pSecondPictureOf3DMode != NULL)
                        VideoDecCompReturnPicture(p->pDecComp, pSecondPictureOf3DMode);
                }
                pPicture = NULL;
                pSecondPictureOf3DMode = NULL;
            }

            if(p->pDeinterlacePrePicture != NULL)
            {
                VideoDecCompReturnPicture(p->pDecComp, p->pDeinterlacePrePicture);
                p->pDeinterlacePrePicture = NULL;
            }
            
            //* clear the eos flag.
            p->bEosFlag = 0;
            p->nRequsetPictureNum = 0;
            bFirstPictureShowed = 0;
            bFirstPtsNotified = 0;
            *pReplyValue = 0;
            sem_post(pReplySem);
            
            //* send a message to continue the thread.
            if(p->eStatus == PLAYER_STATUS_STARTED ||
               (p->eStatus == PLAYER_STATUS_PAUSED && bFirstPictureShowed == 0))
                PostRenderMessage(p->mq);
        }
        else if(msg.messageId == MESSAGE_ID_EOS)
        {
            logi("process MESSAGE_ID_EOS message");
            p->bEosFlag = 1;
            sem_post(pReplySem);
            
            //* send a message to continue the thread.
            if(p->eStatus == PLAYER_STATUS_STARTED ||
               (p->eStatus == PLAYER_STATUS_PAUSED && bFirstPictureShowed == 0))
                PostRenderMessage(p->mq);
        }
        else if(msg.messageId == MESSAGE_ID_SET_WINDOW)
        {
            logi("process MESSAGE_ID_SET_WINDOW message");
            //* return buffer to old layer.
            if(pLayerBuffer != NULL)
            {
                if(p->pLayerCtrl != NULL)
                {
                    if(pSecondLayerBufferOf3DMode == NULL)
                        LayerQueueBuffer(p->pLayerCtrl, pLayerBuffer, 0);
                    else
                        LayerQueue3DBuffer(p->pLayerCtrl, pLayerBuffer, pSecondLayerBufferOf3DMode, 0);
                }
                pLayerBuffer = NULL;
                pSecondLayerBufferOf3DMode = NULL;
            }
            
			if(pPreLayerBuffer != NULL)
			{
				if(p->pLayerCtrl != NULL)
				{
					if(pSecondLayerBufferOf3DMode == NULL)
						LayerQueueBuffer(p->pLayerCtrl, pPreLayerBuffer, 0);
					else
						LayerQueue3DBuffer(p->pLayerCtrl, pPreLayerBuffer, pSecondLayerBufferOf3DMode, 0);
				}
				pPreLayerBuffer = NULL;
				pSecondLayerBufferOf3DMode = NULL;
			}
            if(p->pLayerCtrl != NULL)
            {
                LayerRelease(p->pLayerCtrl, 0);
                p->pLayerCtrl = NULL;
            }
            
            p->pNativeWindow = (void*)msg.params[2];
            
            //* on linux, pNativeWindow == NULL, and the LayerCtrl module will 
            //* create a layer to show video picture.
#if CONFIG_OS == OPTION_OS_ANDROID
            if(p->pNativeWindow != NULL)
#endif
            {
                p->pLayerCtrl = LayerInit(p->pNativeWindow);
                if(p->pLayerCtrl != NULL)
                    LayerSetCallback(p->pLayerCtrl, (PlayerCallback)LayerCallback, (void*)p);
                else
                {
                    loge("can not initialize the video layer.");
                }
            }
            bNeedResetLayerParams = 1;
            sem_post(pReplySem);
            
            //* send a message to continue the thread.
            if(p->eStatus == PLAYER_STATUS_STARTED ||
               (p->eStatus == PLAYER_STATUS_PAUSED && bFirstPictureShowed == 0))
                PostRenderMessage(p->mq);
        }
        else if(msg.messageId == MESSAGE_ID_SET_3D_MODE)
        {
            logi("process MESSAGE_ID_SET_3D_MODE message");
            p->ePicture3DMode = (enum EPICTURE3DMODE)msg.params[2];
            p->eDisplay3DMode = (enum EDISPLAY3DMODE)msg.params[3];
            
            //* now , we no need to set 3D mode to nativeWindow , the app will set it
            #if 0
            if(p->pLayerCtrl != NULL)
            {
                LayerSetPicture3DMode(p->pLayerCtrl, (enum EPICTURE3DMODE)msg.params[2]);
                *pReplyValue = LayerSetDisplay3DMode(p->pLayerCtrl, (enum EDISPLAY3DMODE)msg.params[3]);
            }
            else
            {
                logw("window not set yet, can not set 3d mode.");
                *pReplyValue = -1;
            }
            #else
            *pReplyValue = 0;
            #endif
            
            sem_post(pReplySem);
            
            //* send a message to continue the thread.
            if(p->eStatus == PLAYER_STATUS_STARTED ||
               (p->eStatus == PLAYER_STATUS_PAUSED && bFirstPictureShowed == 0))
                PostRenderMessage(p->mq);
        }
        else if(msg.messageId == MESSAGE_ID_SET_VIDEO_HIDE)
        {
            logi("process MESSAGE_ID_SET_VIDEO_HIDE message");
            bHideVideo = msg.params[2];
            if(bHideVideo == 1) //* hide video.
            {
                if(p->pLayerCtrl != NULL && LayerCtrlIsVideoShow(p->pLayerCtrl) == 1)
                    LayerCtrlHideVideo(p->pLayerCtrl);
            }
            else
            {
                if(p->pLayerCtrl != NULL && 
                   LayerCtrlIsVideoShow(p->pLayerCtrl) == 0 && 
                   bFirstPictureShowed == 1)
                    LayerCtrlShowVideo(p->pLayerCtrl);
            }
            sem_post(pReplySem);
            
            //* send a message to continue the thread.
            if(p->eStatus == PLAYER_STATUS_STARTED ||
               (p->eStatus == PLAYER_STATUS_PAUSED && bFirstPictureShowed == 0))
                PostRenderMessage(p->mq);
        }
        else if(msg.messageId == MESSAGE_ID_SET_HOLD_LAST_PICTURE)
        {
            bHoldLastPicture = msg.params[2];
			
            logv("process MESSAGE_ID_SET_HOLD_LAST_PICTURE message, bHoldLastPicture(%d)", bHoldLastPicture);
            sem_post(pReplySem);
            
            //* send a message to continue the thread.
            if(p->eStatus == PLAYER_STATUS_STARTED ||
               (p->eStatus == PLAYER_STATUS_PAUSED && bFirstPictureShowed == 0))
                PostRenderMessage(p->mq);
        }
        else if(msg.messageId == MESSAGE_ID_RENDER)
        {
            logi("process MESSAGE_ID_RENDER message");
            
            if(p->eStatus != PLAYER_STATUS_STARTED && 
              !(p->eStatus == PLAYER_STATUS_PAUSED && bFirstPictureShowed == 0))
            {
                logw("not in started status, render message ignored.");
                continue;
            }
            
            //****************************
            //* check whether it is a 3D stream.
            //****************************
            if(bFirstPictureShowed == 0)
            {
                do
                {
                    bVideoWithTwoStream = IsVideoWithTwoStream(p->pDecComp);
                    if(bVideoWithTwoStream == -1)
                    {
                        //* get stream info fail, decoder not initialized yet.
                        bVideoWithTwoStream = 0;
                        
                        //* check whether stream end.
                        if(p->bEosFlag)
                            p->callback(p->pUserData, PLAYER_VIDEO_RENDER_NOTIFY_EOS, NULL);
                
                        ret = MessageQueueTryGetMessage(p->mq, &msg, 10); //* wait for 10ms if no message come.
                        if(ret == 0)    //* new message come, quit loop to process.
                            goto process_message;
                    }
                    else
                        break;
                }while(1);
            }
            
            if(pPicture == NULL && pLayerBuffer == NULL)
            {
                //*******************************
                //* 1. get picture from decoder.
                //*******************************
                if(bVideoWithTwoStream == 0)
                {
                    //* get one picture from decoder.
                    while(pPicture == NULL)
                    {
                        pPicture = VideoDecCompRequestPicture(p->pDecComp, 0, &bResolutionChange);
                        if(pPicture != NULL || p->bEosFlag)
                        {
                            p->nRequsetPictureNum++;
                            break;
                        }

                        if(bResolutionChange)
                        {
                            bNeedResetAngleFlag = 1;
                            //* reopen the layer.
                            if(p->pLayerCtrl != NULL)
                            {
                                if(bFirstPictureShowed == 1)
                                    LayerCtrlHoldLastPicture(p->pLayerCtrl, 1);
                                LayerRelease(p->pLayerCtrl, 0);
                                p->pLayerCtrl = NULL;
                                
                                p->pLayerCtrl = LayerInit(p->pNativeWindow);
                                if(p->pLayerCtrl != NULL)
                                {
                                    LayerSetCallback(p->pLayerCtrl, (PlayerCallback)LayerCallback, (void*)p);
                                    bNeedResetLayerParams = 1;
                                }
                            }

                            if(p->pDeinterlacePrePicture != NULL)
                            {
                                VideoDecCompReturnPicture(p->pDecComp, p->pDeinterlacePrePicture);
                                p->pDeinterlacePrePicture = NULL;
                            }
                            //* reopen the video engine.
                            VideoDecCompReopenVideoEngine(p->pDecComp);
                        }

                        ret = MessageQueueTryGetMessage(p->mq, &msg, 5); //* wait for 5ms if no message come.
                        if(ret == 0)    //* new message come, quit loop to process.
                            goto process_message;
                    }
                }
                else
                {
                    //* get pictures of two streams  from decoder.
                    while(pPicture == NULL)
                    {
                        if(VideoDecCompNextPictureInfo(p->pDecComp, 0, 0) != NULL &&
                        		VideoDecCompNextPictureInfo(p->pDecComp, 1, 0) != NULL)
                        {
                            pPicture = VideoDecCompRequestPicture(p->pDecComp, 0);
                            pSecondPictureOf3DMode = VideoDecCompRequestPicture(p->pDecComp, 1);
                        }

                        if(pPicture != NULL || p->bEosFlag)
                            break;
                            
                        ret = MessageQueueTryGetMessage(p->mq, &msg, 5); //* wait for 10ms if no message come.
                        if(ret == 0)    //* new message come, quit loop to process.
                            goto process_message;
                    }
                }

                //* debug show videoDecodefps
                showVideoDecodefps(pPicture);

                //*****************************************************************
                //* 2. handle EOS, pPicture should not be NULL except bEosFlag==1.
                //*****************************************************************
                if(pPicture == NULL)
                {
                    if(p->bEosFlag == 1)
                    {
                        p->callback(p->pUserData, PLAYER_VIDEO_RENDER_NOTIFY_EOS, NULL);
                        continue;
                    }
                    else
                    {
                        loge("pPicture=NULL but bEosFlag is not set, shouldn't run here.");
                        abort();
                    }
                }
                
                //********************************************
                //* 3. initialize layer and notify video size.
                //********************************************
                if(bFirstPictureShowed == 0 || bNeedResetLayerParams == 1)
                {
					int size[4];

                    //* We should compute the real width and height again if the offset is valid.
                    //* Because the pPicture->nWidth is not the real width, it is buffer widht.
					if((pPicture->nBottomOffset != 0 || pPicture->nRightOffset != 0) &&
						pPicture->nRightOffset <= pPicture->nLineStride)
					{
						size[0] = pPicture->nRightOffset - pPicture->nLeftOffset;
						size[1] = pPicture->nBottomOffset - pPicture->nTopOffset;
						size[2] = 0;
						size[3] = 0;
					}
					else
					{
						size[0] = pPicture->nWidth;
						size[1] = pPicture->nHeight;
						size[2] = 0;
						size[3] = 0;
					}
                    p->callback(p->pUserData, PLAYER_VIDEO_RENDER_NOTIFY_VIDEO_SIZE, (void*)size);

                    if((pPicture->nRightOffset - pPicture->nLeftOffset) > 0 && 
                       (pPicture->nBottomOffset - pPicture->nTopOffset) > 0)
		        	{
	        		    size[0] = pPicture->nLeftOffset;
					    size[1] = pPicture->nTopOffset;
		        		size[2] = pPicture->nRightOffset - pPicture->nLeftOffset;
						size[3] = pPicture->nBottomOffset - pPicture->nTopOffset;
                        p->callback(p->pUserData, PLAYER_VIDEO_RENDER_NOTIFY_VIDEO_CROP, (void*)size);
		        	}
		        	
                    if(p->pLayerCtrl)
                    {
                        //* we use rotate-transform to do video-rotation on 1673,
                        //* we set the PIXEL_FORMAT_YV12 , becase the output format of
                        //* rotate-transform is YV12
                        #if(CONFIG_CHIP == OPTION_CHIP_1673 && CONFIG_PRODUCT == OPTION_PRODUCT_PAD)
                            LayerSetExpectPixelFormat(p->pLayerCtrl, PIXEL_FORMAT_YV12);
                        #else
                            LayerSetExpectPixelFormat(p->pLayerCtrl,(enum EPIXELFORMAT)pPicture->ePixelFormat);
                        #endif

                        //* if use deinterlace, decise by if DeinterlaceCreate() success
                        if (p->di
                           && pPicture->bIsProgressive == 0
                           && USE_DETNTERLACE == 1)
                        {
                            if(p->pDeinterlacePrePicture != NULL)
                            {
                                VideoDecCompReturnPicture(p->pDecComp, p->pDeinterlacePrePicture);
                                p->pDeinterlacePrePicture = NULL;
                            }
                            
                            if (p->di->init() == 0)
                            {
                                int di_flag = p->di->flag();
                                p->nRequsetPictureNum = 1;//*have requset picture here
                                bDeinterlaceFlag      = 1;
                                //nDeinterlaceDispNum   = (di_flag == DE_INTERLACE_HW) ? 2 : 1;
                                if (MemAdapterGetDramFreq() < 360000 && pPicture->nHeight >= 1080) {
                                    nDeinterlaceDispNum = 1;
                                } else if (di_flag == DE_INTERLACE_HW) {
                                    nDeinterlaceDispNum = 2;
                                } else {
                                    nDeinterlaceDispNum = 1;
                                }
                                LayerSetRenderToHardwareFlag(p->pLayerCtrl,0);
                                LayerSetExpectPixelFormat(p->pLayerCtrl, p->di->expectPixelFormat());
                                LayerSetDeinterlaceFlag(p->pLayerCtrl, p->di->flag());
                            }
                            else
                            {
                                logw(" open deinterlace failed , we not to use deinterlace!");
                            }

                        }
                        else
                        {
                            nDeinterlaceDispNum = 1;
                        }
                        //nDeinterlaceDispNum = 1;
                        
                        LayerSetPictureSize(p->pLayerCtrl, pPicture->nWidth, pPicture->nHeight);
                        LayerSetDisplayRegion(p->pLayerCtrl,
                        		              pPicture->nLeftOffset,
                        		              pPicture->nTopOffset,
                        		              pPicture->nRightOffset - pPicture->nLeftOffset,
                        		              pPicture->nBottomOffset - pPicture->nTopOffset);
                        bNeedResetLayerParams = 0;
                    }
                }
                
                //************************************************************************************
                //* 4. notify the first sync frame to set timer. the first sync frame is the second
                //*    picture, the first picture need to be showed as soon as we can.(unsynchroized)
                //************************************************************************************
step_4:
                if(bFirstPictureShowed == 1
					|| (bFirstPictureShowed == 0 && p->bSyncFirstPictureFlag == 1))
                {
                    if(bFirstPtsNotified == 0)
                    {
	                    //* this callback may block because the player need wait audio first frame to sync.
	                    ret = p->callback(p->pUserData, PLAYER_VIDEO_RENDER_NOTIFY_FIRST_PICTURE, (void*)&pPicture->nPts);
	                    if(ret == TIMER_DROP_VIDEO_DATA)
	                    {
	                        //* video first frame pts small (too much) than the audio, 
	                        //* discard this frame to catch up the audio.
                            VideoDecCompReturnPicture(p->pDecComp, pPicture);
                            pPicture = NULL;
                            if(pSecondPictureOf3DMode != NULL)
                            {
                                VideoDecCompReturnPicture(p->pDecComp, pSecondPictureOf3DMode);
                                pSecondPictureOf3DMode = NULL;
                            }
                            PostRenderMessage(p->mq);
                            continue;
	                    }
                        else if(ret == TIMER_NEED_NOTIFY_AGAIN)
                        {
                            //* waiting process for first frame sync with audio is broken by a new message to player, so the player tell us to notify again later.
                            //* post a render message to continue the rendering job after message processed.
                            ret = MessageQueueTryGetMessage(p->mq, &msg, 10); //* wait for 10ms if no message come.
                            if(ret == 0)    //* new message come, quit loop to process.
                                goto process_message;
                            PostRenderMessage(p->mq);
                            continue;
                        }
	                    bFirstPtsNotified = 1;
                    }
                }

#if(CONFIG_CHIP == OPTION_CHIP_1673 && CONFIG_PRODUCT == OPTION_PRODUCT_PAD)
                //* 4.1 on 1673, we use rotate-transfrom to do video rotation
                //*     so, we should check the system angle and reset the Layer
                if(bFirstPictureShowed == 0 || bNeedResetAngleFlag == 1)
                    CheckScreenRotateAngle(p, pPicture, 1);
                else
                    CheckScreenRotateAngle(p, pPicture, 0);

                if(bNeedResetAngleFlag == 1)
                    bNeedResetAngleFlag = 0;
#endif
                //******************************************************
                //* 5. dequeue buffer from layer and copy picture data.
                //******************************************************
step_5:
                logv("***nDeinterlaceDispNum = %d,bDeinterlaceFlag = %d",nDeinterlaceDispNum,bDeinterlaceFlag);
                for(int nDeinterlaceTime = 0;nDeinterlaceTime < nDeinterlaceDispNum;nDeinterlaceTime++)
                {
                    if(pLayerBuffer == NULL)
                    {
                        if(bVideoWithTwoStream == 0)
                        {
                            //* dequeue buffer from layer.
                            //* if the pLayerCtrl==NULL(when the nativeWindow==NULL),
                            //* we should not call LayerDequeueBuffer();
                            if(p->pLayerCtrl != NULL)
                            {
                                while(pLayerBuffer == NULL)
                                {
                                    // i) acquire buffer for deinterlace.
                                    //    buffer from a) dequeue buffer
                                    //                b) last non-queue buffer(pPreLayerBuffer).
                                    if(pPreLayerBuffer != NULL)
                                    {
                                        pLayerBuffer    = pPreLayerBuffer;
                                        pPreLayerBuffer = NULL;
                                    }
                                    else
                                        nLayerBufferMode = LayerDequeueBuffer(p->pLayerCtrl, &pLayerBuffer);
                                    
                                    if(nLayerBufferMode == LAYER_RESULT_USE_OUTSIDE_BUFFER)
                                    {
                                        pLayerBuffer = pPicture;
                                        pPicture = NULL;
                                        break;
                                    }
                                    else if(nLayerBufferMode == 0)
                                    {
                                        if(bDeinterlaceFlag == 1)
                                        {
                                            if(p->pDeinterlacePrePicture == NULL)
                                                p->pDeinterlacePrePicture = pPicture;

                                            // ii) deinterlace process
                                            int diret = p->di->process(p->pDeinterlacePrePicture,
                                                               pPicture,
                                                               pLayerBuffer,
                                                               nDeinterlaceTime);
                                            if (diret != 0)
                                            {
                                                p->di->reset();

                                                // iii) deinterlace error handling.
                                                //      two ways for handling pLayerBuffer,
                                                //          a) cancel to layer
                                                //          b) set to pPreLayerBuffer for next deinterlace process.
                                                //      we use b).
                                                if (nDeinterlaceTime == (nDeinterlaceDispNum - 1)) {
                                                    // last field, quit render msg.
                                                    logd("last field...");
                                                    if(pPicture != p->pDeinterlacePrePicture)
                                                        VideoDecCompReturnPicture(p->pDecComp, p->pDeinterlacePrePicture);
                                                    p->pDeinterlacePrePicture = pPicture;
                                                    pPreLayerBuffer = pLayerBuffer;
                                                    pLayerBuffer = NULL;
                                                    goto process_message;
                                                } else {
                                                    // first field, try deinterlace last field.
                                                    logd("first field...");
													// prepicture == curpicture
                                                    if(pPicture != p->pDeinterlacePrePicture)
                                                        VideoDecCompReturnPicture(p->pDecComp, p->pDeinterlacePrePicture);
                                                    p->pDeinterlacePrePicture = pPicture;
                                                    pPreLayerBuffer = pLayerBuffer;
                                                    pLayerBuffer = NULL;
                                                    nDeinterlaceTime ++;
                                                    continue;
                                                }
                                            }

                                            // iv) end of deinterlacing this frame, set prepicture for next frame.
                                            if(nDeinterlaceTime == (nDeinterlaceDispNum - 1))
                                            {
                                                if(pPicture != p->pDeinterlacePrePicture 
                                                   && p->pDeinterlacePrePicture != NULL)
                                                {
                                                    VideoDecCompReturnPicture(p->pDecComp, p->pDeinterlacePrePicture);
                                                }
                                                p->pDeinterlacePrePicture = pPicture;
                                                pPicture = NULL;
                                            }
                                            
                                        }
                                        else
                                        {
			                                VideoDecCompRotatePicture(p->pDecComp, 
                                                                      pPicture, 
                                                                      pLayerBuffer, 
                                                                      p->nRotationAngle,
                                                                      p->nGpuYAlign,
                                                                      p->nGpuCAlign);  //* copy picture.
			                                VideoDecCompReturnPicture(p->pDecComp, pPicture);
			                                pPicture = NULL;
                                        }
                                        break;
                                    }
                                    else
                                    {
                                        loge("dequeue buffer from layer fail.");
                                        ret = MessageQueueTryGetMessage(p->mq, &msg, 5); //* wait for 5ms if no message come.
                                        if(ret == 0)    //* new message come, quit loop to process.
                                            goto process_message;
                                    }
                                }
                            }
                            else
                                pLayerBuffer = pPicture;
                        }
                        else
                        {
                            //* dequeue buffer from layer.
                            //* if the pLayerCtrl==NULL(when the nativeWindow==NULL),
                            //* we should not call LayerDequeueBuffer();
                            if(p->pLayerCtrl != NULL)
                            {
                                while(pLayerBuffer == NULL)
                                {
                                	if(pPreLayerBuffer != NULL)
                                    {
                                        pLayerBuffer    = pPreLayerBuffer;
                                        pPreLayerBuffer = NULL;
                                    }
                                    else
                                    	nLayerBufferMode = LayerDequeue3DBuffer(p->pLayerCtrl, &pLayerBuffer, &pSecondLayerBufferOf3DMode);

									if(nLayerBufferMode == LAYER_RESULT_USE_OUTSIDE_BUFFER)
                                    {
                                        pLayerBuffer = pPicture;
                                        pSecondLayerBufferOf3DMode = pSecondPictureOf3DMode;
                                        pPicture = NULL;
                                        pSecondPictureOf3DMode = NULL;
                                        break;
                                    }
                                    else if(nLayerBufferMode == 0)
                                    {
                                        //* copy picture.
                                        VideoDecCompRotatePicture(p->pDecComp, 
                                                                  pPicture, 
                                                                  pLayerBuffer, 
                                                                  p->nRotationAngle,
                                                                  p->nGpuYAlign,
                                                                  p->nGpuCAlign);
                                		VideoDecCompRotatePicture(p->pDecComp, 
                                                                  pSecondPictureOf3DMode, 
                                                                  pSecondLayerBufferOf3DMode, 
                                                                  p->nRotationAngle,
                                                                  p->nGpuYAlign,
                                                                  p->nGpuCAlign);
                                        VideoDecCompReturnPicture(p->pDecComp, pPicture);
                                        VideoDecCompReturnPicture(p->pDecComp, pSecondPictureOf3DMode);
                                        pPicture = NULL;
                                        pSecondPictureOf3DMode = NULL;
                                        break;
                                    }
                                    else
                                    {
                                        loge("dequeue buffer from layer fail.");
                                        ret = MessageQueueTryGetMessage(p->mq, &msg, 5); //* wait for 5ms if no message come.
                                        if(ret == 0)    //* new message come, quit loop to process.
                                            goto process_message;
                                    }
                                }
                            }
                            else
                            {
                                pLayerBuffer = pPicture;
                                pSecondLayerBufferOf3DMode = pSecondPictureOf3DMode;
                            }
                        }
                    }

                   
                    //******************************************************
                    //* 6. wait according to the presentation time stamp.
                    //******************************************************
//step_6:
                    //* the first picture is showed unsychronized if the flag is 0
                    if(bFirstPictureShowed == 1
					   || (bFirstPictureShowed == 0 && p->bSyncFirstPictureFlag == 1))
                    {
                        //* nWaitTime is in unit of ms.
                        nWaitTime = p->callback(p->pUserData, PLAYER_VIDEO_RENDER_NOTIFY_PICTURE_PTS, (void*)&pLayerBuffer->nPts);
						if (SEND_PTS_TO_SF == 1)
						{
							if (nWaitTime > 120)
							{
								nWaitTime -= 90;
							}
							else if(nWaitTime >= 0)
							{
								nWaitTime = 0;
							}
						}
						#if (CONFIG_CHIP == OPTION_CHIP_1680)
						ANativeWindow *pNativeWindow = (ANativeWindow *)p->pNativeWindow;
						int dispFPS = pNativeWindow->perform(pNativeWindow, NATIVE_WINDOW_GETPARAMETER, DISPLAY_CMD_GETDISPFPS);
						int frameRate = pLayerBuffer->nFrameRate;
						int dropTime = -2500;
						if (frameRate == 0)
							frameRate = 25;
						if (frameRate > 1000)
							frameRate = (frameRate + 999) / 1000;
						if (frameRate > dispFPS)
							dropTime = -100;
						#else
							int dropTime = -2500;
						#endif
						//logd("dropTime=%d", dropTime);
                        if(nWaitTime > 0)
                        {
                            int nWaitTimeOnce;
                            while(nWaitTime > 0)
                            {
                                //* wait for 100ms if no message come.
                                nWaitTimeOnce = (nWaitTime>100 ? 100 : nWaitTime);
                                ret = MessageQueueTryGetMessage(p->mq, &msg, nWaitTimeOnce);
                                if(ret == 0)    //* new message come, quit loop to process.
                                {
                                    //* if pLayerCtrl==null, we should return picture before process message
                                    //* or the picture will never be returned.
                                    if(p->pLayerCtrl == NULL)
                                    {
                                        if(bVideoWithTwoStream == 0)
                                        {
                                            VideoDecCompReturnPicture(p->pDecComp, pPicture);
                                            pPicture = NULL;
                                            pLayerBuffer = NULL;
                                        }
                                        else
                                        {
                                            VideoDecCompReturnPicture(p->pDecComp, pPicture);
                                            VideoDecCompReturnPicture(p->pDecComp, pSecondPictureOf3DMode);
                                            pPicture = NULL;
                                            pSecondPictureOf3DMode = NULL;
                                            pLayerBuffer = NULL;
                                            pSecondLayerBufferOf3DMode = NULL;
                                        }
                                    }
                                    goto process_message;
                                }
                                nWaitTime -= nWaitTimeOnce;
                            }
                        }
                        else if(nWaitTime < -100 && bDeinterlaceFlag == 1)
                        {
							//* if it is deinterlace and expired, we should drop it
                            #if 1
                            pPreLayerBuffer = pLayerBuffer;
                            pLayerBuffer    = NULL;
                            if(nDeinterlaceTime == 0)
                                continue;
                            else
                                break;
                            #endif
                                
                        }
						else if(nWaitTime < dropTime && bDeinterlaceFlag == 0)
						{					
							loge("*** the picture is too late(%d ms), drop it",nWaitTime);
							if(bVideoWithTwoStream == 0)
	                        {
	                        	if(nLayerBufferMode == LAYER_RESULT_USE_OUTSIDE_BUFFER)
	                        	{
									VideoDecCompReturnPicture(p->pDecComp, pLayerBuffer);
									pLayerBuffer = NULL;
								}
								pPreLayerBuffer = pLayerBuffer;
	                            pPicture 		= NULL;
	                            pLayerBuffer 	= NULL;
	                        }
	                        else
	                        {
	                        	if(nLayerBufferMode == LAYER_RESULT_USE_OUTSIDE_BUFFER)
	                        	{
	                            	VideoDecCompReturnPicture(p->pDecComp, pLayerBuffer);
	                            	VideoDecCompReturnPicture(p->pDecComp, pSecondLayerBufferOf3DMode);
									pLayerBuffer = NULL;
									pSecondLayerBufferOf3DMode = NULL;
	                        	}
								pPreLayerBuffer = pLayerBuffer;
	                            pPicture 		= NULL;
	                            pSecondPictureOf3DMode = NULL;
	                            pLayerBuffer    = NULL;
	                            pSecondLayerBufferOf3DMode = NULL;
	                        }
	                        break;	
						}
                    }
                    
                    //******************************************************
                    //* 7. queue buffer to show.
                    //******************************************************
                    if(p->pLayerCtrl != NULL)
                    {
#if(CONFIG_CMCC==OPTION_CMCC_NO)
                    	if ((p->pAvTimer != NULL)
							&& (SEND_PTS_TO_SF == 1))
                    	{
	                        int64_t ptsAbs = p->pAvTimer->PtsToSystemTime(p->pAvTimer, pLayerBuffer->nPts);
	                        LayerSetBufferTimeStamp(p->pLayerCtrl, ptsAbs);
                    	}
#endif
                    	
                        if(bVideoWithTwoStream == 0)
                        {
                            LayerQueueBuffer(p->pLayerCtrl, pLayerBuffer);
                            pLayerBuffer = NULL;
                        }
                        else
                        {
                            LayerQueue3DBuffer(p->pLayerCtrl, pLayerBuffer, pSecondLayerBufferOf3DMode);
                            pLayerBuffer = NULL;
                            pSecondLayerBufferOf3DMode = NULL;
                        }
                    }
                    else
                    {
                        //* if pLayerCtrl==null , we should return picture to decoder immediately
                        if(bVideoWithTwoStream == 0)
                        {
                            VideoDecCompReturnPicture(p->pDecComp, pPicture);
                            pPicture = NULL;
                            pLayerBuffer = NULL;
                        }
                        else
                        {
                            VideoDecCompReturnPicture(p->pDecComp, pPicture);
                            VideoDecCompReturnPicture(p->pDecComp, pSecondPictureOf3DMode);
                            pPicture = NULL;
                            pSecondPictureOf3DMode = NULL;
                            pLayerBuffer = NULL;
                            pSecondLayerBufferOf3DMode = NULL;
                        }
                    }
    				
                    if(p->pLayerCtrl != NULL && 
                       LayerCtrlIsVideoShow(p->pLayerCtrl) == 0 && 
                       bHideVideo == 0)
                        LayerCtrlShowVideo(p->pLayerCtrl);
                    

            	}

				
                if(bFirstPictureShowed == 0)
                    bFirstPictureShowed = 1;
                
                if(p->eStatus == PLAYER_STATUS_STARTED)
                    PostRenderMessage(p->mq);
                else
                {
                    //* p->eStatus == PLAYER_STATUS_PAUSED && bFirstPictureShowed == 0
                    //* need to show the first picture as soon as we can after seek.
                    logi("first picture showed at paused status.");
                }
                continue;
            }
            else    //* pPicture or pLayerBuffer not NULL.
            {
                //* continue last process.
                if(pLayerBuffer != NULL)
                    goto step_5;    
                else if(bFirstPtsNotified == 1)
                    goto step_5;
                else
                    goto step_4;
            }
        }
        else
        {
            //* unknown message.
            if(pReplyValue != NULL)
                *pReplyValue = -1;
            if(pReplySem)
                sem_post(pReplySem);
        }
    }
    
    if(p->pLayerCtrl != NULL)
    {
        LayerRelease(p->pLayerCtrl, 0);
        p->pLayerCtrl = NULL;
    }
    
    ret = 0;
    pthread_exit(&ret);
    
    return NULL;
}


static void PostRenderMessage(MessageQueue* mq)
{
    if(MessageQueueGetCount(mq)<=0)
    {
        Message msg;
        msg.messageId = MESSAGE_ID_RENDER;
        msg.params[0] = msg.params[1] = msg.params[2] = msg.params[3] = 0;
        if(MessageQueuePostMessage(mq, &msg) != 0)
        {
            loge("fatal error, video render component post message fail.");
            abort();
        }
        
        return;
    }
}

static int IsVideoWithTwoStream(VideoDecComp* pDecComp)
{
    VideoStreamInfo videoStreamInfo;
    if(VideoDecCompGetVideoStreamInfo(pDecComp, &videoStreamInfo) == 0)
        return videoStreamInfo.bIs3DStream;
    else
        return -1;
}


static int LayerCallback(void* pUserData, int eMessageId, void* param)
{
    VideoPicture* pPicture;
    VideoRenderCompContext* p;
    
    p = (VideoRenderCompContext*)pUserData;
    if(eMessageId == MESSAGE_ID_LAYER_RETURN_BUFFER)
    {
        pPicture = (VideoPicture*)param;
        if(p->pDecComp != NULL)
        {
            VideoDecCompReturnPicture(p->pDecComp, pPicture);
        }
    }
    
    return 0;
}

static void CheckScreenRotateAngle(VideoRenderCompContext* p,
                                   VideoPicture* pPicture,
                                   int bNeedResetAngleFlag)
{
    if(p->pLayerCtrl != NULL)
    {   
        int nCurRotation = LayerGetRotationAngle(p->pLayerCtrl);
        logv("**nCurRotation = %d",nCurRotation);
        if(p->nRotationAngle != nCurRotation || bNeedResetAngleFlag == 1)
        {
            logv("**angle change : %d,  %d",p->nRotationAngle,nCurRotation);
            int nRotateWidth         = pPicture->nWidth;
            int nRotateHeight        = pPicture->nHeight;
            int nRotateTopOffset     = 0;
            int nRotateLeftOffset    = 0;
            int nRotateDisplayWidth  = 0;
            int nRotateDisplayHeight = 0;

            if(nCurRotation == 0)
            {
                nRotateWidth         = pPicture->nWidth;
                nRotateHeight        = pPicture->nHeight;
                nRotateTopOffset     = pPicture->nTopOffset;
                nRotateLeftOffset    = pPicture->nLeftOffset;
                nRotateDisplayWidth  = pPicture->nRightOffset - pPicture->nLeftOffset;
                nRotateDisplayHeight = pPicture->nBottomOffset - pPicture->nTopOffset;
            }
            else if(nCurRotation == 90)
            {
                nRotateWidth         = pPicture->nHeight;
                nRotateHeight        = pPicture->nWidth;
                nRotateTopOffset     = pPicture->nLeftOffset;
                nRotateLeftOffset    = pPicture->nHeight - pPicture->nBottomOffset;
                nRotateDisplayWidth  = pPicture->nBottomOffset - pPicture->nTopOffset;
                nRotateDisplayHeight = pPicture->nRightOffset - pPicture->nLeftOffset;
            }
            else if(nCurRotation == 180)
            {
                nRotateWidth         = pPicture->nWidth;
                nRotateHeight        = pPicture->nHeight;
                nRotateTopOffset     = pPicture->nHeight - pPicture->nBottomOffset;
                nRotateLeftOffset    = pPicture->nLineStride- pPicture->nRightOffset;
                nRotateDisplayWidth  = pPicture->nRightOffset - pPicture->nLeftOffset;
                nRotateDisplayHeight = pPicture->nBottomOffset - pPicture->nTopOffset;
            }
            else if(nCurRotation == 270)
            {
                nRotateWidth         = pPicture->nHeight;
                nRotateHeight        = pPicture->nWidth;
                nRotateTopOffset     = pPicture->nLineStride - pPicture->nRightOffset;
                nRotateLeftOffset    = pPicture->nTopOffset;
                nRotateDisplayWidth  = pPicture->nBottomOffset - pPicture->nTopOffset;
                nRotateDisplayHeight = pPicture->nRightOffset - pPicture->nLeftOffset;
            }
            else
            {
                loge("the nCurRotation is not 0, 90, 180, 270! is--%d",nCurRotation);
            }

            logv("nRotate %d rect info : w[%d], h[%d], l[%d], t[%d], dw[%d], dh[%d],realRrightO[%d],realBottom[%d]",
                  nCurRotation,
                  nRotateWidth,
                  nRotateHeight,
                  nRotateLeftOffset,
                  nRotateTopOffset,
                  nRotateDisplayWidth,
                  nRotateDisplayHeight,
                  pPicture->nRightOffset,
                  pPicture->nBottomOffset
                  );
            //* reset the Layer
            LayerSetPictureSize(p->pLayerCtrl, nRotateWidth, nRotateHeight);

            //* we should not the region when nRightOffset or nBottomOffset is 0
            if(pPicture->nRightOffset != 0 && pPicture->nBottomOffset != 0)
            {
                LayerSetDisplayRegion(p->pLayerCtrl,
                                      nRotateLeftOffset,
                                      nRotateTopOffset,
                                      nRotateDisplayWidth,
                                      nRotateDisplayHeight);
            }
            p->nRotationAngle = nCurRotation;
        }
            
    }
}


//*****************************************************************
//* showVideoDecodefps debug Instructions:
//* open debug: "adb shell setprop debug.cdx.videoDecodefps 1"
//* close debug: "adb shell setprop debug.cdx.videoDecodefps 0"
//*****************************************************************
static void showVideoDecodefps(VideoPicture* pPicture)
{
	char property[PROPERTY_VALUE_MAX]={0};
	int  show_fps_settings = 0;
	if (property_get("debug.cdx.videoDecodefps", property, NULL) >= 0)
	{
		if(property[0] == '1')
			show_fps_settings = 1;
	}
	else
	{
		logd("No videoDecodefps debug attribute node.");
		return;
	}

	if (pPicture != NULL && show_fps_settings == 1)
	{
		static int64_t nDebugLastTimeMs = systemTime()/1000000;
		static int nFrameCnt = 0;
		nFrameCnt++;
		if (systemTime()/1000000 - nDebugLastTimeMs >= 1000)
		{
			logd("VideoDecodefps %dfps", nFrameCnt);
			nDebugLastTimeMs = systemTime()/1000000;
			nFrameCnt = 0;
		}
	}
}
