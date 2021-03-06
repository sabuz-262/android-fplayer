/*
 * mediaplayer.cpp
 */

//#define LOG_NDEBUG 0
#define TAG "FFMpegMediaPlayer"

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

extern "C" {
	
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/log.h"
	
} // end of extern C

#include <android/log.h>

#include "mediaplayer.h"
#include "output.h"


#define FPS_DEBUGGING false

static MediaPlayer* sPlayer;
MediaPlayer::MediaPlayer()
{
    mListener = NULL;
    mCookie = NULL;
    mDuration = -1;
    mStreamType = MUSIC;
    mCurrentPosition = -1;
    mSeekPosition = -1;
    mCurrentState = MEDIA_PLAYER_IDLE;
    mPrepareSync = false;
    mPrepareStatus = NO_ERROR;
    mLoop = false;
    pthread_mutex_init(&mLock, NULL);
    mLeftVolume = mRightVolume = 1.0;
    mVideoWidth = mVideoHeight = 0;
    sPlayer = this;
	vp     = new VideoPicture;
	frame_timer=0;
	frame_last_pts=0;
	frame_last_delay=0;
	video_clock=0;
}

MediaPlayer::~MediaPlayer()
{
	if(mListener != NULL) {
		free(mListener);
	}
	if(vp)
		free(vp);
}

status_t MediaPlayer::prepareAudio()
{
	//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareAudio");
	mAudioStreamIndex = -1;
	for (int i = 0; i < mMovieFile->nb_streams; i++) {
		if (mMovieFile->streams[i]->codec->codec_type == CODEC_TYPE_AUDIO) {
			mAudioStreamIndex = i;
			break;
		}
	}
	
	if (mAudioStreamIndex == -1) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareAudio INVALID_OPERATION 1");
		return INVALID_OPERATION;
	}

	AVStream* stream = mMovieFile->streams[mAudioStreamIndex];
	// Get a pointer to the codec context for the video stream
	AVCodecContext* codec_ctx = stream->codec;
	AVCodec* codec = avcodec_find_decoder(codec_ctx->codec_id);
	if (codec == NULL) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareAudio INVALID_OPERATION 2");
		return INVALID_OPERATION;
	}
	
	// Open codec
	if (avcodec_open(codec_ctx, codec) < 0) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareAudio INVALID_OPERATION 3");
		return INVALID_OPERATION;
	}

	// prepare os output
	if (Output::AudioDriver_set(MUSIC,
								stream->codec->sample_rate,
								PCM_16_BIT,
								(stream->codec->channels == 2) ? CHANNEL_OUT_STEREO
										: CHANNEL_OUT_MONO) != ANDROID_AUDIOTRACK_RESULT_SUCCESS) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareAudio INVALID_OPERATION 4");
		return INVALID_OPERATION;
	}

	if (Output::AudioDriver_start() != ANDROID_AUDIOTRACK_RESULT_SUCCESS) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareAudio INVALID_OPERATION 5");
		return INVALID_OPERATION;
	}

	//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareAudio NO_ERROR");
	return NO_ERROR;
}

status_t MediaPlayer::prepareVideo()
{
	//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareVideo");
	// Find the first video stream
	mVideoStreamIndex = -1;
	for (int i = 0; i < mMovieFile->nb_streams; i++) {
		if (mMovieFile->streams[i]->codec->codec_type == CODEC_TYPE_VIDEO) {
			mVideoStreamIndex = i;
			break;
		}
	}
	
	if (mVideoStreamIndex == -1) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareVideo INVALID_OPERATION");
		return INVALID_OPERATION;
	}
	
	AVStream* stream = mMovieFile->streams[mVideoStreamIndex];
	// Get a pointer to the codec context for the video stream
	AVCodecContext* codec_ctx = stream->codec;
	AVCodec* codec = avcodec_find_decoder(codec_ctx->codec_id);
	if (codec == NULL) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareVideo INVALID_OPERATION 1");
		return INVALID_OPERATION;
	}
	
	// Open codec
	if (avcodec_open(codec_ctx, codec) < 0) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareVideo  INVALID_OPERATION 2" );
		return INVALID_OPERATION;
	}
	
	mVideoWidth = codec_ctx->width;
	mVideoHeight = codec_ctx->height;
	mDuration =  mMovieFile->duration;
	
	//PIX_FMT_YUV420P  PIX_FMT_RGB565
	mConvertCtx = sws_getContext(stream->codec->width,
								 stream->codec->height,
								 stream->codec->pix_fmt,
								 stream->codec->width,
								 stream->codec->height,
								 PIX_FMT_RGB565,
								 SWS_POINT,
								 NULL,
								 NULL,
								 NULL);

	if (mConvertCtx == NULL) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareVideo  INVALID_OPERATION 3" );
		return INVALID_OPERATION;
	}

	void*		pixels;
	if (Output::VideoDriver_getPixels(stream->codec->width,
									  stream->codec->height,
									  &pixels) != ANDROID_SURFACE_RESULT_SUCCESS) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareVideo  INVALID_OPERATION 4" );
		return INVALID_OPERATION;
	}

	mFrame = avcodec_alloc_frame();
	if (mFrame == NULL) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareVideo  INVALID_OPERATION 5" );
		return INVALID_OPERATION;
	}
	// Assign appropriate parts of buffer to image planes in pFrameRGB
	// Note that pFrameRGB is an AVFrame, but AVFrame is a superset
	// of AVPicture
	//PIX_FMT_YUV420P PIX_FMT_RGB565
	avpicture_fill((AVPicture *) mFrame,
				   (uint8_t *) pixels,
				   PIX_FMT_RGB565,
				   stream->codec->width,
				   stream->codec->height);
	//__android_log_print(ANDROID_LOG_ERROR, TAG, "prepareVideo  NO_ERROR" );

	return NO_ERROR;
}

status_t MediaPlayer::prepare()
{
	status_t ret;
	mCurrentState = MEDIA_PLAYER_PREPARING;
	//av_log_set_callback(ffmpegNotify);
	if ((ret = prepareVideo()) != NO_ERROR) {
		mCurrentState = MEDIA_PLAYER_STATE_ERROR;
		return ret;
	}



	if ((ret = prepareAudio()) != NO_ERROR) {
		mCurrentState = MEDIA_PLAYER_STATE_ERROR;
		return ret;
	}

	mCurrentState = MEDIA_PLAYER_PREPARED;
	return NO_ERROR;
}

status_t MediaPlayer::setListener(MediaPlayerListener* listener)
{
    __android_log_print(ANDROID_LOG_INFO, TAG, "setListener");
    mListener = listener;
    return NO_ERROR;
}

status_t MediaPlayer::setDataSource(const char *url)
{
   // __android_log_print(ANDROID_LOG_INFO, TAG, "setDataSource(%s)", url);
    status_t err = BAD_VALUE;
	// Open video file
	if(av_open_input_file(&mMovieFile, url, NULL, 0, NULL) != 0) {
		return INVALID_OPERATION;
	}
	// Retrieve stream information
	if(av_find_stream_info(mMovieFile) < 0) {
		return INVALID_OPERATION;
	}
	mCurrentState = MEDIA_PLAYER_INITIALIZED;
    return NO_ERROR;
}

status_t MediaPlayer::suspend() {
	//__android_log_print(ANDROID_LOG_INFO, TAG, "suspend");
	
	mCurrentState = MEDIA_PLAYER_STOPPED;
	if(mDecoderAudio != NULL) {
		mDecoderAudio->stop();
	}
	if(mDecoderVideo != NULL) {
		mDecoderVideo->stop();
	}
	
	/*if(pthread_join(mRenderThread, NULL) != 0) {
		__android_log_print(ANDROID_LOG_ERROR, TAG, "Couldn't cancel render thread");
	}*/
	if(pthread_join(mPlayerThread, NULL) != 0) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "Couldn't cancel player thread");
	}
	
	// Close the codec
	free(mDecoderAudio);
	free(mDecoderVideo);
	
	// Close the video file
	av_close_input_file(mMovieFile);

	//close OS drivers
	Output::AudioDriver_unregister();
	Output::VideoDriver_unregister();

	//__android_log_print(ANDROID_LOG_ERROR, TAG, "suspended");

    return NO_ERROR;
}

status_t MediaPlayer::resume() {
	//pthread_mutex_lock(&mLock);
	mCurrentState = MEDIA_PLAYER_STARTED;
	//pthread_mutex_unlock(&mLock);
    return NO_ERROR;
}

status_t MediaPlayer::setVideoSurface(JNIEnv* env, jobject jsurface)
{ 
	if(Output::VideoDriver_register(env, jsurface) != ANDROID_SURFACE_RESULT_SUCCESS) {
		return INVALID_OPERATION;
	}
	if(Output::AudioDriver_register() != ANDROID_AUDIOTRACK_RESULT_SUCCESS) {
		return INVALID_OPERATION;
	}	
    return NO_ERROR;
}

bool MediaPlayer::shouldCancel(PacketQueue* queue)
{
	return (mCurrentState == MEDIA_PLAYER_STATE_ERROR || mCurrentState == MEDIA_PLAYER_STOPPED ||
			 ((mCurrentState == MEDIA_PLAYER_DECODED || mCurrentState == MEDIA_PLAYER_STARTED) 
			  && queue->size() == 0));
}

/**
 * handler for receiving decoded video frames
 */
void MediaPlayer::decode(AVFrame* frame, double pts)
{
	if(FPS_DEBUGGING) {
		timeval pTime;
		static int frames = 0;
		static double t1 = -1;
		static double t2 = -1;

		gettimeofday(&pTime, NULL);
		t2 = pTime.tv_sec + (pTime.tv_usec / 1000000.0);
		if (t1 == -1 || t2 > t1 + 1) {
			//__android_log_print(ANDROID_LOG_INFO, TAG, "Video fps:%i", frames);
			//sPlayer->notify(MEDIA_INFO_FRAMERATE_VIDEO, frames, -1);
			t1 = t2;
			frames = 0;
		}
		frames++;
	}

	/*
	AVFrame* f = avcodec_alloc_frame();
	if (f == NULL) {
		return;
	}
	// Assign appropriate parts of buffer to image planes in pFrameRGB
	// Note that pFrameRGB is an AVFrame, but AVFrame is a superset
	// of AVPicture
	uint8_t *pixels = (uint8_t *) malloc(sizeof(uint8_t *) * sPlayer->mVideoWidth * sPlayer->mVideoHeight * 2);
	avpicture_fill((AVPicture *) f,
				   (uint8_t *) pixels,
				   PIX_FMT_RGB565,
				   sPlayer->mVideoWidth,
				   sPlayer->mVideoHeight);

	memcpy(f->data, frame->data, sPlayer->mVideoWidth * sPlayer->mVideoHeight * 2);

	__android_log_print(ANDROID_LOG_INFO, TAG, "1");

	f->linesize[0] = frame->linesize[0];
	f->linesize[1] = frame->linesize[1];
	f->linesize[2] = frame->linesize[2];
	f->linesize[3] = frame->linesize[3];
**/
	sPlayer->showPreview(frame, pts);
}


void MediaPlayer::showPreview(AVFrame* frame, double pts)
{
	while (mVideoQueue.size()>0&&mCurrentState != MEDIA_PLAYER_DECODED && mCurrentState != MEDIA_PLAYER_STOPPED &&
		   mCurrentState != MEDIA_PLAYER_STATE_ERROR) {
		usleep(2*10000);
	}
	
    vp->frame=frame;
	vp->pts=pts;
	
	sPlayer->mVideoQueue.push(vp);
	
	__android_log_print(ANDROID_LOG_INFO, TAG, "2");
	
}


void MediaPlayer::add_db_time(double pts)
{
	double delay,ref_clock,diff,actual_delay=0;
	delay = pts - frame_last_pts; /* the pts from last time */
	if(delay <= 0 || delay >= 1.0) {
		/* if incorrect delay, use previous one */
		delay = frame_last_delay;
	}
	
	/* save for next time */
	frame_last_delay = delay;
	frame_last_pts = pts;
	
	/* update delay to sync to audio 
     ref_clock = get_audio_clock(is);
     diff = pts - ref_clock;
	 
	 
     sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
     if(fabs(diff) < AV_NOSYNC_THRESHOLD) {
	 if(diff <= -sync_threshold) {
	 delay = 0;
	 } else if(diff >= sync_threshold) {
	 delay = 2 * delay;
	 }
     }
	 */
	frame_timer += delay;
	/* computer the REAL delay */
	actual_delay = frame_timer - (av_gettime() / 1000000.0);
	if(actual_delay < 0.010) {
		/* Really it should skip the picture instead */
		actual_delay = 0.010;
	}
	//schedule_refresh(is, (int)(actual_delay * 1000 + 0.5));
	__android_log_print(ANDROID_LOG_INFO, TAG, "delay: %f",delay);
	__android_log_print(ANDROID_LOG_INFO, TAG, "frame_last_delay: %f",frame_last_delay);
    __android_log_print(ANDROID_LOG_INFO, TAG, "frame_timer: %f",frame_timer);
	__android_log_print(ANDROID_LOG_INFO, TAG, "actual_delay:%f",actual_delay);
	__android_log_print(ANDROID_LOG_INFO, TAG, "pts:%f",pts);
    long ldelay=delay*100000;
	ldelay=ldelay==0?1:ldelay;
	__android_log_print(ANDROID_LOG_INFO, TAG, "ldelay:%ld",ldelay);
	//sPlayer->vp->frame=frame;
	//vp->frame=frame;
	// Convert the image from its native format to RGB
	//video_display(NULL,pts);
	//SetTimer()
	
	int res = 0;
	// Register printMsg to SIGALRM
	signal(SIGALRM, time_update_video);
	  
	struct itimerval tick;
	// Initialize struct
    memset(&tick, 0, sizeof(tick));
	// Timeout to run function first time
	tick.it_value.tv_sec = 0;  // sec
    tick.it_value.tv_usec = ldelay; // micro sec.
	// Interval time to run function
	//tick.it_interval.tv_sec = 1;
	//tick.it_interval.tv_usec = 0;
    // Set timer, ITIMER_REAL : real-time to decrease timer,
	//                          send SIGALRM when timeout
	res = setitimer(ITIMER_REAL, &tick, NULL);
	if (res) 
	{
		printf("Set timer failed!!\n");
	}
}

void MediaPlayer::time_update_video(int num)
{
	sPlayer->render(NULL);

}

void MediaPlayer::video_display(AVFrame *frame,double pts)
{
	//vp->frame;
	sws_scale(sPlayer->mConvertCtx,
		      	sPlayer->vp->frame->data,
		      	sPlayer->vp->frame->linesize,
			  0,
			  sPlayer->mVideoHeight,
			  sPlayer->mFrame->data,
			  sPlayer->mFrame->linesize);

	Output::VideoDriver_updateSurface();
}

/**
 * handler for receiving decoded audio buffers
 */
void MediaPlayer::decode(int16_t* buffer, int buffer_size)
{
	if(FPS_DEBUGGING) {
		timeval pTime;
		static int frames = 0;
		static double t1 = -1;
		static double t2 = -1;

		gettimeofday(&pTime, NULL);
		t2 = pTime.tv_sec + (pTime.tv_usec / 1000000.0);
		if (t1 == -1 || t2 > t1 + 1) {
			//__android_log_print(ANDROID_LOG_INFO, TAG, "Audio fps:%i", frames);
			//sPlayer->notify(MEDIA_INFO_FRAMERATE_AUDIO, frames, -1);
			t1 = t2;
			frames = 0;
		}
		frames++;
	}

	if(Output::AudioDriver_write(buffer, buffer_size) <= 0) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "Couldn't write samples to audio track");
	}
}

void MediaPlayer::decodeMovie(void* ptr)
{
	AVPacket pPacket;
	
	AVStream* stream_audio = mMovieFile->streams[mAudioStreamIndex];
	mDecoderAudio = new DecoderAudio(stream_audio);
	mDecoderAudio->onDecode = decode;
	mDecoderAudio->startAsync();
	
	AVStream* stream_video = mMovieFile->streams[mVideoStreamIndex];
	mDecoderVideo = new DecoderVideo(stream_video);
	mDecoderVideo->onDecode = decode;
	mDecoderVideo->startAsync();
	frame_timer = (double)av_gettime() / 1000000.0;
    frame_last_delay = 40e-3;
	
	mCurrentState = MEDIA_PLAYER_STARTED;
	//__android_log_print(ANDROID_LOG_INFO, TAG, "playing %ix%i", mVideoWidth, mVideoHeight);
	while (mCurrentState != MEDIA_PLAYER_DECODED && mCurrentState != MEDIA_PLAYER_STOPPED &&
		   mCurrentState != MEDIA_PLAYER_STATE_ERROR)
	{
		if (mDecoderVideo->packets() > FFMPEG_PLAYER_MAX_QUEUE_SIZE &&
				mDecoderAudio->packets() > FFMPEG_PLAYER_MAX_QUEUE_SIZE) {
			usleep(200);
			continue;
		}
		
		if(av_read_frame(mMovieFile, &pPacket) < 0) {
			mCurrentState = MEDIA_PLAYER_DECODED;
			continue;
		}
		
		// Is this a packet from the video stream?
		if (pPacket.stream_index == mVideoStreamIndex) {
			mDecoderVideo->enqueue(&pPacket);
		} 
		else if (pPacket.stream_index == mAudioStreamIndex) {
			mDecoderAudio->enqueue(&pPacket);
		}
		else {
			// Free the packet that was allocated by av_read_frame
			av_free_packet(&pPacket);
		}
	}
	
	//waits on end of video thread
	//__android_log_print(ANDROID_LOG_ERROR, TAG, "waiting on video thread");
	int ret = -1;
	if((ret = mDecoderVideo->wait()) != 0) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "Couldn't cancel video thread: %i", ret);
	}
	
	__android_log_print(ANDROID_LOG_ERROR, TAG, "waiting on audio thread");
	if((ret = mDecoderAudio->wait()) != 0) {
		//__android_log_print(ANDROID_LOG_ERROR, TAG, "Couldn't cancel audio thread: %i", ret);
	}
    
	if(mCurrentState == MEDIA_PLAYER_STATE_ERROR) {
		//__android_log_print(ANDROID_LOG_INFO, TAG, "playing err");
	}
	mCurrentState = MEDIA_PLAYER_PLAYBACK_COMPLETE;
	//__android_log_print(ANDROID_LOG_INFO, TAG, "end of playing");
}

void MediaPlayer::render(void* ptr)
{
	
	/**
	while (mCurrentState != MEDIA_PLAYER_DECODED && mCurrentState != MEDIA_PLAYER_STOPPED &&
			   mCurrentState != MEDIA_PLAYER_STATE_ERROR)
	{
		int length = mVideoQueue.size();
		if(length > 0) {
		   // AVFrame** frames = mVideoQueue.editArray();
			VideoPicture** vps = mVideoQueue.editArray();
		    mVideoQueue.clear();
		    for(int i=0;i<length;i++) {
		    	//AVFrame* frame = frames[i];
				VideoPicture* cvp = vps[i];

		    	//__android_log_print(ANDROID_LOG_INFO, TAG, "3");

		    	// Convert the image from its native format to RGB
		    	sws_scale(sPlayer->mConvertCtx,
		    		      cvp->frame->data,
		    		      cvp->frame->linesize,
		    			  0,
		    			  mVideoHeight,
		    			  mFrame->data,
		    			  mFrame->linesize);

		    	Output::VideoDriver_updateSurface();
		    	//av_free(frame);

		    	//__android_log_print(ANDROID_LOG_INFO, TAG, "4");
		    }
		}
		usleep(20*10000);
	}**/
	if(mCurrentState == MEDIA_PLAYER_DECODED || mCurrentState == MEDIA_PLAYER_STOPPED ||
		mCurrentState == MEDIA_PLAYER_STATE_ERROR)
	{
		   return ;
    }
	while (mCurrentState != MEDIA_PLAYER_DECODED && mCurrentState != MEDIA_PLAYER_STOPPED &&
			   mCurrentState != MEDIA_PLAYER_STATE_ERROR&&mVideoQueue.size()==0) {
		usleep(20);
	}
			  
	int length = mVideoQueue.size();
		   if(length > 0) {
			   // AVFrame** frames = mVideoQueue.editArray();
			   VideoPicture** vps = mVideoQueue.editArray();
			   mVideoQueue.clear();
			   for(int i=0;i<length;i++) {
				   //AVFrame* frame = frames[i];
				   VideoPicture* cvp = vps[i];
				   
				   __android_log_print(ANDROID_LOG_INFO, TAG, "3");
				   
				   // Convert the image from its native format to RGB
				   sws_scale(sPlayer->mConvertCtx,
							 cvp->frame->data,
							 cvp->frame->linesize,
							 0,
							 mVideoHeight,
							 mFrame->data,
							 mFrame->linesize);
				   
				   Output::VideoDriver_updateSurface();
				   //av_free(frame);
				   add_db_time(cvp->pts);

				   __android_log_print(ANDROID_LOG_INFO, TAG, "4");
			   }
		   }
		   
}

void* MediaPlayer::startPlayer(void* ptr)
{
   // __android_log_print(ANDROID_LOG_INFO, TAG, "starting main player thread");

    sPlayer->decodeMovie(ptr);
}

void* MediaPlayer::startRendering(void* ptr)
{
	//__android_log_print(ANDROID_LOG_INFO, TAG, "starting rendering thread");
	sPlayer->render(ptr);
}

status_t MediaPlayer::start()
{
	if (mCurrentState != MEDIA_PLAYER_PREPARED) {
		return INVALID_OPERATION;
	}
	pthread_create(&mPlayerThread, NULL, startPlayer, NULL);
	pthread_create(&mRenderThread, NULL, startRendering, NULL);
	return NO_ERROR;
}

status_t MediaPlayer::stop()
{
	pthread_mutex_lock(&mLock);
	mCurrentState = MEDIA_PLAYER_STOPPED;
	pthread_mutex_unlock(&mLock);
    return NO_ERROR;
}

status_t MediaPlayer::pause()
{
	pthread_mutex_lock(&mLock);
	mCurrentState = MEDIA_PLAYER_PAUSED;
	pthread_mutex_unlock(&mLock);
	return NO_ERROR;
}

bool MediaPlayer::isPlaying()
{
    return mCurrentState == MEDIA_PLAYER_STARTED || 
		mCurrentState == MEDIA_PLAYER_DECODED;
}

status_t MediaPlayer::getVideoWidth(int *w)
{
	if (mCurrentState < MEDIA_PLAYER_PREPARED) {
		return INVALID_OPERATION;
	}
	*w = mVideoWidth;
    return NO_ERROR;
}

status_t MediaPlayer::getVideoHeight(int *h)
{
	if (mCurrentState < MEDIA_PLAYER_PREPARED) {
		return INVALID_OPERATION;
	}
	*h = mVideoHeight;
    return NO_ERROR;
}

status_t MediaPlayer::getCurrentPosition(int *msec)
{
	if (mCurrentState < MEDIA_PLAYER_PREPARED) {
		return INVALID_OPERATION;
	}
	*msec = 0/*av_gettime()*/;
	//__android_log_print(ANDROID_LOG_INFO, TAG, "position %i", *msec);
	return NO_ERROR;
}

status_t MediaPlayer::getDuration(int *msec)
{
	if (mCurrentState < MEDIA_PLAYER_PREPARED) {
		return INVALID_OPERATION;
	}
	*msec = mDuration / 1000;
    return NO_ERROR;
}

status_t MediaPlayer::seekTo(int msec)
{
    return INVALID_OPERATION;
}

status_t MediaPlayer::reset()
{
    return INVALID_OPERATION;
}

status_t MediaPlayer::setAudioStreamType(int type)
{
	return NO_ERROR;
}

void MediaPlayer::ffmpegNotify(void* ptr, int level, const char* fmt, va_list vl) {
	
	switch(level) {
			/**
			 * Something went really wrong and we will crash now.
			 */
		case AV_LOG_PANIC:
			__android_log_print(ANDROID_LOG_ERROR, TAG, "AV_LOG_PANIC: %s", fmt);
			//sPlayer->mCurrentState = MEDIA_PLAYER_STATE_ERROR;
			break;
			
			/**
			 * Something went wrong and recovery is not possible.
			 * For example, no header was found for a format which depends
			 * on headers or an illegal combination of parameters is used.
			 */
		case AV_LOG_FATAL:
			__android_log_print(ANDROID_LOG_ERROR, TAG, "AV_LOG_FATAL: %s", fmt);
			//sPlayer->mCurrentState = MEDIA_PLAYER_STATE_ERROR;
			break;
			
			/**
			 * Something went wrong and cannot losslessly be recovered.
			 * However, not all future data is affected.
			 */
		case AV_LOG_ERROR:
			__android_log_print(ANDROID_LOG_ERROR, TAG, "AV_LOG_ERROR: %s", fmt);
			//sPlayer->mCurrentState = MEDIA_PLAYER_STATE_ERROR;
			break;
			
			/**
			 * Something somehow does not look correct. This may or may not
			 * lead to problems. An example would be the use of '-vstrict -2'.
			 */
		case AV_LOG_WARNING:
			__android_log_print(ANDROID_LOG_ERROR, TAG, "AV_LOG_WARNING: %s", fmt);
			break;
			
		case AV_LOG_INFO:
			__android_log_print(ANDROID_LOG_INFO, TAG, "%s", fmt);
			break;
			
		case AV_LOG_DEBUG:
			__android_log_print(ANDROID_LOG_DEBUG, TAG, "%s", fmt);
			break;
			
	}
}

void MediaPlayer::notify(int msg, int ext1, int ext2)
{
    //__android_log_print(ANDROID_LOG_INFO, TAG, "message received msg=%d, ext1=%d, ext2=%d", msg, ext1, ext2);
    bool send = true;
    bool locked = false;

    if ((mListener != 0) && send) {
       //__android_log_print(ANDROID_LOG_INFO, TAG, "callback application");
       mListener->notify(msg, ext1, ext2);
       //__android_log_print(ANDROID_LOG_INFO, TAG, "back from callback");
	}
}
