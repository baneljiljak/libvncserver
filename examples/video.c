
/**
 * @example video.c
 *
 * Read raw video. Pixel format is rgba. Send image from video as desktop image.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rfb/rfb.h>
#ifdef LIBVNCSERVER_HAVE_GETTIMEOFDAY
/* if we have gettimeofday(), it is in this header */
#include <sys/time.h>
#endif
#if !defined LIBVNCSERVER_HAVE_GETTIMEOFDAY && defined WIN32
#include <fcntl.h>
#include <conio.h>
#include <sys/timeb.h>

static void gettimeofday(struct timeval* tv,char* dummy)
{
   SYSTEMTIME t;
   GetSystemTime(&t);
   tv->tv_sec=t.wHour*3600+t.wMinute*60+t.wSecond;
   tv->tv_usec=t.wMilliseconds*1000;
}
#endif


//#define WIDTH  640
//#define HEIGHT 480
#define BPP      4

/* 15 frames per second (if we can) */
//#define PICTURE_TIMEOUT (1.0/60.0)


/*
 * throttle camera updates
*/
int TimeToTakePicture(int fps) {
    static struct timeval now={0,0}, then={0,0};
    double picture_timeout = 1/((double)fps);
    double elapsed, dnow, dthen;

    gettimeofday(&now,NULL);

    dnow  = now.tv_sec  + (now.tv_usec /1000000.0);
    dthen = then.tv_sec + (then.tv_usec/1000000.0);
    elapsed = dnow - dthen;

    if (elapsed > picture_timeout)
      memcpy((char *)&then, (char *)&now, sizeof(struct timeval));
    return elapsed > picture_timeout;
}



/*
 * simulate grabbing a picture from some device
 */
int TakePicture(char *buffer, FILE *fp, int width, int height)
{
  return fread(buffer, sizeof(char), width*height*BPP, fp);
}




/* 
 * Single-threaded application that interleaves client servicing with taking
 * pictures from the camera.  This way, we do not update the framebuffer
 * while an encoding is working on it too (banding, and image artifacts).
 */
int main(int argc,char** argv)
{                                       
  long usec;
  char *filename;
  int width;
  int height;
  int fps;
  FILE *fp;
 
  if(argc != 5) {
    printf("Usage: %s input.raw height width fps\n", argv[0]);
    return 0;
  }
 
  filename = argv[1];
  width    = atoi(argv[2]);
  height   = atoi(argv[3]);
  fps      = atoi(argv[4]);

  rfbScreenInfoPtr server=rfbGetScreen(&argc,argv,width,height,8,3,BPP);
  if(!server)
    return 1;
  server->desktopName = "Live Video Feed Example";
  server->frameBuffer=(char*)malloc(width*height*BPP);
  server->alwaysShared=(1==1);

  /* Initialize the server */
  rfbInitServer(server);           

  fp = fopen(filename, "rb");
  if(fp == NULL) {
    printf("Unable to open file: %s\n", filename);
    return 2;
  }

  /* Loop, processing clients and taking pictures */
  while (rfbIsActive(server)) {
    if (TimeToTakePicture(fps)) {
      server->frameBuffer[0] = 'a';
      if(TakePicture(server->frameBuffer, fp, width, height))
        rfbMarkRectAsModified(server,0,0,width,height);
      else {
        printf("No data was read from file.\n");
        return 3;
      }
    }
    usec = server->deferUpdateTime*1000;
    rfbProcessEvents(server,usec);
  }
  return(0);
}
