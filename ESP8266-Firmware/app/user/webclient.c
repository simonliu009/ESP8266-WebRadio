#include "webclient.h"
#include "webserver.h"

#include "lwip/sockets.h"
#include "lwip/api.h"
#include "lwip/netdb.h"

#include "esp_common.h"

#include "freertos/semphr.h"

#include "vs1053.h"
#include "eeprom.h"

static enum clientStatus cstatus;
static uint32_t metacount = 0;
static uint16_t metasize = 0;

xSemaphoreHandle sConnect, sConnected, sDisconnect, sHeader;

static uint8_t connect = 0, playing = 0;


/* TODO:
	- METADATA HANDLING
	- IP SETTINGS
	- VS1053 - DELAY USING vTaskDelay
*/
struct icyHeader header = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL};
int headerlen[ICY_HEADER_COUNT] = {0,0,0,0,0,0,0,0,0,0};

char *metaint = NULL;
char *clientURL = NULL;
char *clientPath = NULL;
uint16_t clientPort = 80;

struct hostent *server;
int rest ;
///////////////
#define BUFFER_SIZE 10240
//#define BUFFER_SIZE 9600
//#define BUFFER_SIZE 8000
uint8_t buffer[BUFFER_SIZE];
uint16_t wptr = 0;
uint16_t rptr = 0;
uint8_t bempty = 1;

void *incmalloc(size_t n)
{
	void* ret;
//printf ("Client malloc of %d,  Heap size: %d\n",n,xPortGetFreeHeapSize( ));
	ret = malloc(n);
		if (ret == NULL) printf("Client: incmalloc fails for %d\n",n);
//	printf ("Client malloc after of %d bytes ret:%x  Heap size: %d\n",n,ret,xPortGetFreeHeapSize( ));
	return ret;
}	
void incfree(void *p)
{
	free(p);
//	printf ("Client incfree of %x,  Heap size: %d\n",p,xPortGetFreeHeapSize( ));
}	

ICACHE_FLASH_ATTR uint16_t getBufferFree() {
	if(wptr > rptr ) return BUFFER_SIZE - wptr + rptr;
	else if(wptr < rptr) return rptr - wptr;
	else if(bempty) return BUFFER_SIZE; else return 0;
}

ICACHE_FLASH_ATTR uint16_t getBufferFilled() {
	return BUFFER_SIZE - getBufferFree();
}

ICACHE_FLASH_ATTR uint16_t bufferWrite(uint8_t *data, uint16_t size) {
//	uint16_t s = size;
	uint16_t i = 0;
	for(i=0; i<size; i++) {
		if(getBufferFree() == 0) { return i;}
		buffer[wptr++] = data[i];
		if(bempty) bempty = 0;
//		wptr++;
		if(wptr == BUFFER_SIZE) wptr = 0;
	}
	return size;
}

ICACHE_FLASH_ATTR uint16_t bufferRead(uint8_t *data, uint16_t size) {
	uint16_t s = size;
	size = (size>>6)<<6; //mod 32
	uint16_t i = 0;
	uint16_t bf = BUFFER_SIZE - getBufferFree();
	if(s > bf) s = bf;
	for (i = 0; i < s; i++) {
		if(bf == 0) { return i;}
		data[i] = buffer[rptr++];
//		rptr++;
		if(rptr == BUFFER_SIZE) rptr = 0;
		if(rptr == wptr) bempty = 1;
	}
	return s;
}

ICACHE_FLASH_ATTR void bufferReset() {
	playing = 0;	
	wptr = 0;
	rptr = 0;
	bempty = 1;
}

///////////////

ICACHE_FLASH_ATTR void clientInit() {
	vSemaphoreCreateBinary(sHeader);
	vSemaphoreCreateBinary(sConnect);
	vSemaphoreCreateBinary(sConnected);
	vSemaphoreCreateBinary(sDisconnect);
	xSemaphoreTake(sConnect, portMAX_DELAY);
	xSemaphoreTake(sConnected, portMAX_DELAY);
	xSemaphoreTake(sDisconnect, portMAX_DELAY);
}

ICACHE_FLASH_ATTR uint8_t clientIsConnected() {
	if(xSemaphoreTake(sConnected, 0)) {
		xSemaphoreGive(sConnected);
		return 0;
	}
	return 1;
}

ICACHE_FLASH_ATTR struct icyHeader* clientGetHeader()
{	
	return &header;
}

	
ICACHE_FLASH_ATTR bool clientParsePlaylist(char* s)
{
  char* str; 
  char path[80] = "/";
  char url[80]; 
  char port[5] = "80";
  int remove;
  int i = 0; int j = 0;
  str = strstr(s,"<location>http://");  //for xspf
  if (str != NULL) remove = 17;
  if (str ==NULL) 
  {	  
	str = strstr(s,"http://");
	if (str != NULL) remove = 7;
  }
  if (str != NULL)
  {
	str += remove; //skip http://
	while ((str[i] != '/')&&(str[i] != ':')&&(str[i] != 0x0a)&&(str[i] != 0x0d)) {url[j] = str[i]; i++ ;j++;}
	url[j] = 0;
	j = 0;
	if (str[i] == ':')  //port
	{
		i++;
		while ((str[i] != '/')&&(str[i] != 0x0a)&&(str[i] != 0x0d)) {port[j] = str[i]; i++ ;j++;}
	}
	j = 0;
	if ((str[i] != 0x0a)&&(str[i] != 0x0d)&&(str[i] != 0)&&(str[i] != '"')&&(str[i] != '<'))
	{	
	  while ((str[i] != 0x0a)&&(str[i] != 0x0d)&&(str[i] != 0)&&(str[i] != '"')&&(str[i] != '<')) {path[j] = str[i]; i++; j++;}
	  path[j] = 0;
	}

	if (strncmp(url,"localhost",9)!=0) clientSetURL(url);
	clientSetPath(path);
	clientSetPort(atoi(port));
	printf("##CLI.URL#: %s, path: %s, port: %s\n",url,path,port);
	return true;
  }
  else 
  { 
   cstatus = C_DATA;
   return false;
  }
}
ICACHE_FLASH_ATTR char* stringify(char* str,int *len)
{
		if ((strchr(str,'"') == NULL)&&(strchr(str,'/') == NULL)) return str;
		char* new = incmalloc(strlen(str)+100);
		if (new != NULL)
		{
//			printf("stringify: enter: len:%d\n",*len);
			int i=0 ,j =0;
			for (i = 0;i< strlen(str)+100;i++) new[i] = 0;
			for (i=0;i< strlen(str);i++)
			{
				if (str[i] == '"') {
					new[j++] = '\\';
				}
				if (str[i] == '/') {
					new[j++] = '\\';
				}
				new[j++] =(str)[i] ;
			}
			incfree(str);
			if (j+1>*len)*len = j+1;
			return new;		
		} else 
		{
			printf("stringify malloc fails\n");
		}	
		return str;
}

ICACHE_FLASH_ATTR void clientSaveMetadata(char* s,int len,bool catenate)
{
	    int oldlen = 0;
		char* t_end = NULL;
		char* t_quote;
		char* t ;
		bool found = false;
//		printf("Entry meta s= %s\n",s);
		if (catenate) oldlen = strlen(header.members.mArr[METADATA]);
		t = s;
		t_end = strstr(t,";StreamUrl='");
		if (t_end != NULL) { *t_end = 0;found = true;} 
		t = strstr(t,"StreamTitle='");
		if (t!= NULL) {t += 13;found = true;} else t = s;
		len = strlen(t);
//		printf("Len= %d t= %s\n",len,t);
		if ((t_end != NULL)&&(len >=3)) t_end -= 3;
		else if (len >3) t_end = t+len-3; else t_end = t;
		if (found)
		{	
			t_quote = strstr(t_end,"'");
			if (t_quote !=NULL){ t_end = t_quote; *t_end = 0;}
		} else {t = "";len = 0;}
//		printf("clientsaveMeta t= 0x%x t_end= 0x%x  t=%s\n",t,t_end,t);
		
//		s = t;
		if((header.members.mArr[METADATA] != NULL)&&(headerlen[METADATA] < (oldlen+len+1)*sizeof(char))) 
		{	// realloc if new incmalloc is bigger (avoid heap fragmentation)
//			printf("clientsaveMeta incfree  %d < %d\n",headerlen[METADATA],(oldlen+len+1)*sizeof(char));
			incfree(header.members.mArr[METADATA]);
			header.members.mArr[METADATA] = (char*)incmalloc((oldlen  +len+1)*sizeof(char));
			headerlen[METADATA] = (oldlen +len+1)*sizeof(char);
		} //else
		if(header.members.mArr[METADATA] == NULL) {
			header.members.mArr[METADATA] = (char*)incmalloc((oldlen  +len+1)*sizeof(char));
			if(header.members.mArr[METADATA] != NULL) 
				headerlen[METADATA] = (oldlen +len+1)*sizeof(char);
			else { headerlen[METADATA] = 0; printf("clientsaveMeta malloc fails\n"); return;}
		}
		int i;
		char* md;
		header.members.mArr[METADATA][oldlen +len] = 0;
		strncpy(&(header.members.mArr[METADATA][oldlen]), t,len);
		md = stringify(header.members.mArr[METADATA],&headerlen[METADATA]);
		if (md != header.members.mArr[METADATA]) incfree (header.members.mArr[METADATA]);
		header.members.mArr[METADATA] = md;
		printf("##CLI.META#: %s\n",header.members.mArr[METADATA]);
		char* title = incmalloc(strlen(header.members.mArr[METADATA])+15);
		if (title != NULL)
		{
			sprintf(title,"{\"meta\":\"%s\"}",header.members.mArr[METADATA]); 
			websocketbroadcast(title, strlen(title));
			incfree(title);
		} else printf("clientsaveMeta malloc title fails\n"); 
}	

// websocket: broadcast volume to all client
ICACHE_FLASH_ATTR void wsVol(char* vol)
{
	char answer[16];
	if (vol != NULL)
	{	
		sprintf(answer,"{\"wsvol\":\"%s\"}",vol);
		websocketbroadcast(answer, strlen(answer));
	} 
}	
//websocket: broadcast all icy and meta info to web client.
ICACHE_FLASH_ATTR void wsHeaders()
{
	uint8_t header_num;
	char* wsh = incmalloc(600);
	if (wsh == NULL) {printf("wsHeader malloc fails\n");return;}
	char* not2;
	not2 = header.members.single.notice2;
	if (not2 ==NULL) not2=header.members.single.audioinfo;
	if ((header.members.single.notice2 != NULL)&(strlen(header.members.single.notice2)==0)) not2=header.members.single.audioinfo;
	sprintf(wsh,"{\"wsicy\":{\"descr\":\"%s\",\"meta\":\"%s\",\"name\":\"%s\",\"bitr\":\"%s\",\"url1\":\"%s\",\"not1\":\"%s\",\"not2\":\"%s\",\"genre\":\"%s\"}}",
			(header.members.single.description ==NULL)?"":header.members.single.description,
			(header.members.single.metadata ==NULL)?"":header.members.single.metadata,	
			(header.members.single.name ==NULL)?"":header.members.single.name,
			(header.members.single.bitrate ==NULL)?"":header.members.single.bitrate,
			(header.members.single.url ==NULL)?"":header.members.single.url,
			(header.members.single.notice1 ==NULL)?"":header.members.single.notice1,
			(not2 ==NULL)?"":not2 ,
			(header.members.single.genre ==NULL)?"":header.members.single.genre); 
//	printf("WSH:\"%s\"\n",wsh);
	websocketbroadcast(wsh, strlen(wsh));	
	incfree (wsh);
}	

ICACHE_FLASH_ATTR void clearHeaders()
{
	uint8_t header_num;
	for(header_num=0; header_num<ICY_HEADER_COUNT; header_num++) {
		if(header_num != METAINT) if(header.members.mArr[header_num] != NULL) {
			header.members.mArr[header_num][0] = 0;				
		}
	}
	header.members.mArr[METAINT] = 0;
	wsHeaders();
}
	
ICACHE_FLASH_ATTR bool clientSaveOneHeader(char* t, uint16_t len, uint8_t header_num)
{
	if((header.members.mArr[header_num] != NULL)&&(headerlen[header_num] < (len+1)*sizeof(char))) 
	{	// realloc if new incmalloc is bigger (avoid heap fragmentation)
		incfree(header.members.mArr[header_num]);
		header.members.mArr[header_num] = NULL;
		headerlen[header_num] = 0;
	}
	if(header.members.mArr[header_num] == NULL) 
		header.members.mArr[header_num] = incmalloc((len+1)*sizeof(char));
	if(header.members.mArr[header_num] == NULL)
	{
		printf("clientSaveOneHeader malloc fails\n");
		return false;
	}	
	headerlen[header_num] = (len+1)*sizeof(char);
	int i;
	char *md;
	for(i = 0; i<len+1; i++) header.members.mArr[header_num][i] = 0;
	strncpy(header.members.mArr[header_num], t, len);
	printf("##CLI.ICY%d#: %s\n",header_num,header.members.mArr[header_num]);
	md = stringify(header.members.mArr[header_num],&headerlen[header_num]);
	if (md != header.members.mArr[header_num] ) incfree (header.members.mArr[header_num] );
	header.members.mArr[header_num] = md;
//	printf("header after  addr:0x%x  cont:%s\n",header.members.mArr[header_num],header.members.mArr[header_num]);
	return true;
}

	
ICACHE_FLASH_ATTR bool clientParseHeader(char* s)
{
	// icy-notice1 icy-notice2 icy-name icy-genre icy-url icy-br
	uint8_t header_num;
	bool ret = false;
//	printf("ParseHeader: %s\n",s);
	xSemaphoreTake(sHeader,portMAX_DELAY);
	if ((cstatus != C_HEADER1)&& (cstatus != C_PLAYLIST))// not ended. dont clear
	{
		clearHeaders();
	}
	for(header_num=0; header_num<ICY_HEADERS_COUNT; header_num++)
	{
//				printf("icy deb: %d\n",header_num);		
		char *t;
		t = strstr(s, icyHeaders[header_num]);
		if( t != NULL )
		{
			t += strlen(icyHeaders[header_num]);
			char *t_end = strstr(t, "\r\n");
			if(t_end != NULL)
			{
//				printf("icy in: %d\n",header_num);		
				uint16_t len = t_end - t;
				if(header_num != METAINT) // Text header field
				{
					ret = clientSaveOneHeader(t, len, header_num);
				}
				else // Numerical header field
				{
					if ((metaint != NULL) && ( (headerlen[header_num]) < ((len+1)*sizeof(char)) ))
					{
						incfree (metaint);
						metaint = NULL;
					}
					if (metaint == NULL) { metaint = (char*) incmalloc((len+1)*sizeof(char));headerlen[header_num]= (len+1)*sizeof(char);}
					if (metaint != NULL)
					{					
						int i;
						for(i = 0; i<len+1; i++) metaint[i] = 0;
						strncpy(metaint, t, len);
						header.members.single.metaint = atoi(metaint);
//						printf("MetaInt= %s, Metaint= %d\n",metaint,header.members.single.metaint);
						ret = true;
					} else printf("clientParseHeader malloc fails\n");
//			printf("icy: %s: %d\n",icyHeaders[header_num],header.members.single.metaint);					
				}
			}
		}
	}
	if (ret == true) wsHeaders();
	xSemaphoreGive(sHeader);
	return ret;
}

ICACHE_FLASH_ATTR void clientSetURL(char* url)
{
	int l = strlen(url)+1;
	if ((clientURL != NULL)&&((strlen(clientURL)+1) < l*sizeof(char))) {incfree(clientURL);clientURL = NULL;} //avoid fragmentation
//	if (clientURL != NULL) {incfree(clientURL);clientURL = NULL;}
	if (clientURL == NULL) clientURL = (char*) incmalloc(l*sizeof(char));
	if(clientURL != NULL) 
	{
		strcpy(clientURL, url);
		printf("##CLI.URLSET#: %s\n",clientURL);
	}	
	else printf("clientSetURL malloc fails\n");
}

ICACHE_FLASH_ATTR void clientSetPath(char* path)
{
	int l = strlen(path)+1;
	if ((clientPath != NULL)&&((strlen(clientPath)+1) < l*sizeof(char))){incfree(clientPath); clientPath = NULL;} //avoid fragmentation
//	if(clientPath != NULL) incfree(clientPath);
	if (clientPath == NULL) clientPath = (char*) incmalloc(l*sizeof(char));
	if(clientPath != NULL)
	{
		strcpy(clientPath, path);
		printf("##CLI.PATHSET#: %s\n",clientPath);
	}
	else printf("clientSetPath malloc fails\n");	
}

ICACHE_FLASH_ATTR void clientSetPort(uint16_t port)
{
	clientPort = port;
	printf("##CLI.PORTSET#: %d\n",port);
}

ICACHE_FLASH_ATTR void clientConnect()
{
	cstatus = C_HEADER;
	metacount = 0;
	metasize = 0;
	//if(netconn_gethostbyname(clientURL, &ipAddress) == ERR_OK) {
	if(server) incfree(server);
	if((server = (struct hostent*)gethostbyname(clientURL))) {
		xSemaphoreGive(sConnect);
	} else {
		clientDisconnect();
	}
}

ICACHE_FLASH_ATTR void clientDisconnect()
{
	//connect = 0;
	xSemaphoreGive(sDisconnect);
	printf("##CLI.STOPPED#\n");
	clearHeaders();
}

IRAM_ATTR void clientReceiveCallback(int sockfd, char *pdata, int len)
{
	/* TODO:
		- What if header is in more than 1 data part? // ok now ...
		- Metadata processing // ok
		- Buffer underflow handling (?) ?
	*/
	static int metad ;
	uint16_t l ;
	char* t1;
	bool  icyfound;

//	if (cstatus != C_DATA){printf("cstatus= %d\n",cstatus);  printf("Len=%d, Byte_list = %s\n",len,pdata);}
	switch (cstatus)
	{
	case C_PLAYLIST:
 
        if (!clientParsePlaylist(pdata)) //need more
		  cstatus = C_PLAYLIST1;
		else clientDisconnect();  
    break;
	case C_PLAYLIST1:
       clientDisconnect();		  
        clientParsePlaylist(pdata) ;//more?
		cstatus = C_PLAYLIST;
	break;
	case C_HEADER:
		clearHeaders();
		metad = -1;
		t1 = strstr(pdata, "302 "); 
		if (t1 ==NULL) t1 = strstr(pdata, "301 "); 
		if (t1 != NULL) { // moved to a new address
			if( strcmp(t1,"Found")||strcmp(t1,"Temporarily")||strcmp(t1,"Moved"))
			{
				printf("Header: Moved\n");
				clientDisconnect();
				clientParsePlaylist(pdata);
				cstatus = C_PLAYLIST;				
			}	
			break;
		}
		//no break here
	case C_HEADER1:  // not ended
		{
			cstatus = C_HEADER1;
			do {
				t1 = strstr(pdata, "\r\n\r\n"); // END OF HEADER
//				printf("Header len: %d,  Header: %s\n",len,pdata);
				if ((t1 != NULL) && (t1 <= pdata+len-4)) 
				{
						icyfound = clientParseHeader(pdata);
						if(header.members.single.metaint > 0) 
						metad = header.members.single.metaint;
//						printf("t1: 0x%x, cstatus: %d, icyfound: %d  metad:%d Metaint:%d\n", t1,cstatus, icyfound,metad, header.members.single.metaint); 
						cstatus = C_DATA;				
						VS1053_flush_cancel(1);
						int newlen = len - (t1-pdata) - 4;
						if(newlen > 0) clientReceiveCallback(sockfd,t1+4, newlen);
				} else
				{
					t1 = NULL;
					len += recv(sockfd, pdata+len, RECEIVE-len, 0);
				}
			} while (t1 == NULL);
		}
	break;
	default:		
// -----------	
		rest = 0;
		if(((header.members.single.metaint != 0)&&(len > metad))) {
			l = pdata[metad]*16;
			rest = len - metad  -l -1;
			if (l !=0)
			{
//				printf("len:%d, metad:%d, l:%d, rest:%d, str: %s\n",len,metad, l,rest,pdata+metad+1 );
				if (rest <0) *(pdata+len) = 0; //truncated
				clientSaveMetadata(pdata+metad+1,l,false);
			}				
			while(getBufferFree() < metad){ vTaskDelay(1); /*printf(":");*/}
				bufferWrite(pdata, metad); 
			metad = header.members.single.metaint - rest ; //until next
			if (rest >0)
			{	
				while(getBufferFree() < rest) {vTaskDelay(1); /*printf(".");*/}
					bufferWrite(pdata+len-rest, rest); 
				rest = 0;
			} 	
		} else 
		{	
	        if (rest <0) 
			{
//				printf("Negative len = %d, metad = %d  rest = %d\n",len,metad,rest);
				clientSaveMetadata(pdata,0-rest,true);
				/*buf =pdata+rest;*/ len +=rest;metad += rest; rest = 0;
			}	
			if (header.members.single.metaint != 0) metad -= len;
//			printf("len = %d, metad = %d  metaint= %d\n",len,metad,header.members.single.metaint);
			while(getBufferFree() < len) {vTaskDelay(1); /*printf("-");*/}
			if (len >0) bufferWrite(pdata+rest, len);	
		}
// ---------------			
		if(!playing && (getBufferFree() < BUFFER_SIZE/4)) {
			playing=1;
			printf("##CLI.PLAYING#\n");
		}	
    }
}

#define VSTASKBUF	1600
//1344 1280
IRAM_ATTR void vsTask(void *pvParams) { 
	uint8_t b[VSTASKBUF];

	struct device_settings *device;
	register uint16_t size ,s;
	Delay(100);
	VS1053_Start();
	device = getDeviceSettings();
	Delay(300);

	VS1053_SPI_SpeedUp();
	VS1053_SetVolume(device->vol);	
	VS1053_SetTreble(device->treble);
	VS1053_SetBass(device->bass);
	VS1053_SetTrebleFreq(device->freqtreble);
	VS1053_SetBassFreq(device->freqbass);
	VS1053_SetSpatial(device->spacial);
	incfree(device);	
	while(1) {
//		VS1053_SPI_SpeedUp();
		if(playing) {
			s = 0; 
			size = bufferRead(b, VSTASKBUF); 
			while(s < size) {
				s += VS1053_SendMusicBytes(b+s, size-s);
			} 
			vTaskDelay(1);
		} else 
		{
			VS1053_SPI_SpeedDown();
			vTaskDelay(10);
		}	
	}
}

ICACHE_FLASH_ATTR void clientTask(void *pvParams) {
//1440	for MTU 
	int sockfd, bytes_read;
	struct sockaddr_in dest;
	uint8_t buffer[RECEIVE];
//	uint8_t* buffer= incmalloc(RECEIVE+1);
	printf("WebClient buffer malloc done\n");
	clearHeaders();
	while(1) {
		xSemaphoreGive(sConnected);

		if(xSemaphoreTake(sConnect, portMAX_DELAY)) {

			xSemaphoreTake(sDisconnect, 0);

			sockfd = socket(AF_INET, SOCK_STREAM, 0);
			if(sockfd >= 0) ; //printf("WebClient Socket created\n");
			else printf("WebClient Socket creation failed\n");
			bzero(&dest, sizeof(dest));
			dest.sin_family = AF_INET;
			dest.sin_port = htons(clientPort);
			dest.sin_addr.s_addr = inet_addr(inet_ntoa(*(struct in_addr*)(server -> h_addr_list[0])));

			/*---Connect to server---*/
			if(connect(sockfd, (struct sockaddr*)&dest, sizeof(dest)) >= 0) 
			{
//				printf("WebClient Socket connected\n");
				memset(buffer,0, RECEIVE);
				
				char *t0 = strstr(clientPath, ".m3u");
				if (t0 == NULL)  t0 = strstr(clientPath, ".pls");
				if (t0 == NULL)  t0 = strstr(clientPath, ".xspf");				
				if (t0 != NULL)  // a playlist asked
				{
				  cstatus = C_PLAYLIST;
				  sprintf(buffer, "GET %s HTTP/1.0\r\nHOST: %s\r\n\r\n", clientPath,clientURL); //ask for the playlist
			    } 
				else sprintf(buffer, "GET %s HTTP/1.1\r\nHost: %s\r\nicy-metadata: 1\r\n\r\n", clientPath,clientURL); 
//				printf("st:%d, Client Sent:\n%s\n",cstatus,buffer);
				send(sockfd, buffer, strlen(buffer), 0);
				
				xSemaphoreTake(sConnected, 0);

				do
				{
//					memset(buffer,0, RECEIVE);
					bytes_read = recv(sockfd, buffer, RECEIVE, 0);
					bytes_read += recv(sockfd, buffer+bytes_read, RECEIVE-bytes_read, 0);
					bytes_read += recv(sockfd, buffer+bytes_read, RECEIVE-bytes_read, 0);
//					printf("s:%d   ", bytes_read);
					//if ( bytes_read > 0 )
					{
						clientReceiveCallback(sockfd,buffer, bytes_read);
					}	
					if(xSemaphoreTake(sDisconnect, 0)) break;	
				}
				while ( bytes_read > 0 );
			} else printf("WebClient Socket fails to connect %d\n", errno);
			/*---Clean up---*/
			if (bytes_read == 0 ) 
			{
					clientDisconnect(); 
					clientSaveOneHeader("Not Found", 9,METANAME);
			}//jpc
			bufferReset();
			if (playing) VS1053_flush_cancel(2);
			playing = 0;
			VS1053_flush_cancel(0);
			shutdown(sockfd,SHUT_RDWR);
			vTaskDelay(10);	
			close(sockfd);
//			printf("WebClient Socket closed\n");
			if (cstatus == C_PLAYLIST) 			
			{
			  clientConnect();
			}
		}
	}
}
