#include <ts/ts.h>
#include <ts/remap.h>
#include <string>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <limits.h>


class Intercept_state
{
public:
	Intercept_state()
	{
		output_bytes = 0;
		fp = NULL;
		filelen = 0;
		already = 0;
		buf = NULL;
	}
	
    TSVConn net_vc;
    TSVIO read_vio;
    TSVIO write_vio;

    TSIOBuffer req_buffer;
    TSIOBufferReader req_reader;
    TSIOBuffer resp_buffer;
    TSIOBufferReader resp_reader;

    long output_bytes;
	FILE *fp;
	long filelen;
	long already;
	std::string filename;
	char *buf;
	int fileok;
};

static int stats_dostuff(TSCont contp, TSEvent event, void *edata);

TSReturnCode TSRemapInit(TSRemapInterface* api_info, char* errbuf, int errbuf_size)
{
	return TS_SUCCESS;
}

TSReturnCode TSRemapNewInstance(int argc, char* argv[], void** ih, char* errbuf, int errbuf_size)
{
	std::string *dir = new std::string();
	for(int i=0; i<argc; i++)
		if(strncasecmp(argv[i], "pubdir=", 7) == 0)
			dir->assign(argv[i]+7);

	*ih = (void*)dir;
	return TS_SUCCESS;
}

void TSRemapDeleteInstance(void *ih)
{
	std::string *dir = (std::string*) ih;
	delete dir;
}

TSRemapStatus TSRemapDoRemap(void* ih, TSHttpTxn rh, TSRemapRequestInfo* rri)
{
	std::string *dir = (std::string*) ih;

	const char *constp;
	int len = 0;
	std::string path;
	
	constp = TSUrlPathGet(rri->requestBufp, rri->requestUrl, &len);
	path.assign(constp, len);
	
	Intercept_state *my_state = new Intercept_state;
	my_state->filename = *dir+path;

	TSCont icontp = TSContCreate(stats_dostuff, TSMutexCreate());
	TSContDataSet(icontp, my_state);
	TSHttpTxnIntercept(icontp, rh);
	
	return TSREMAP_DID_REMAP;
}

const int32_t blocksize = 32*1024;
char error404[] = "404 Not Found";

static void stats_cleanup(TSCont contp)
{
	Intercept_state * my_state = (Intercept_state*)TSContDataGet(contp);
	if(my_state)
	{
		if(my_state->fp != NULL)
			fclose(my_state->fp);
		
		if(my_state->buf != NULL)
			delete[] my_state->buf;
		
		if (my_state->req_buffer)
	    {
			TSIOBufferReaderFree(my_state->req_reader);
			my_state->req_reader = NULL;
	        TSIOBufferDestroy(my_state->req_buffer);
	        my_state->req_buffer = NULL;
	    }

	    if (my_state->resp_buffer)
	    {
			TSIOBufferReaderFree(my_state->resp_reader);
			my_state->resp_reader = NULL;
	        TSIOBufferDestroy(my_state->resp_buffer);
	        my_state->resp_buffer = NULL;
	    }
	    TSVConnClose(my_state->net_vc);
	    delete my_state;
	}
    
    TSContDestroy(contp);
}

static void stats_process_accept(TSCont contp, TSEvent event, TSVConn vc)
{
	Intercept_state * my_state = (Intercept_state*)TSContDataGet(contp);
    if(event == TS_EVENT_NET_ACCEPT)
    {
        if(my_state)
        {
			my_state->net_vc = vc;
            my_state->req_buffer = TSIOBufferCreate();
            my_state->req_reader = TSIOBufferReaderAlloc(my_state->req_buffer);
            TSIOBufferWaterMarkSet(my_state->req_buffer, 32*1024);
            my_state->resp_buffer = TSIOBufferCreate();
            my_state->resp_reader = TSIOBufferReaderAlloc(my_state->resp_buffer);
            my_state->read_vio = TSVConnRead(my_state->net_vc, contp, my_state->req_buffer, INT_MAX);
        }
        else
        {
            TSVConnClose(vc);
            TSContDestroy(contp);
        }
    }
	else
	{
        if (my_state)
        {
            delete my_state;
        }
        TSContDestroy(contp);
	}
}

static void stats_process_io(TSCont contp, TSEvent event, void *edata)
{
    Intercept_state * my_state = (Intercept_state*)TSContDataGet(contp);

    switch(event)
    {
	    case TS_EVENT_VCONN_READ_READY:
	    case TS_EVENT_VCONN_READ_COMPLETE:
	    {
	        TSVConnShutdown(my_state->net_vc, 1, 0);
			char buf[512];
			memset(buf, 0, sizeof(buf));
			
			struct stat st;
			if(stat(my_state->filename.c_str(), &st) == 0)
			{
				my_state->filelen = st.st_size;
			}
			
			my_state->fp = fopen(my_state->filename.c_str(), "r");
			my_state->buf = new char[blocksize];
			if(my_state->fp!=NULL && my_state->filelen>0)
			{
				std::string mimetype();
				
				sprintf(buf, "HTTP/1.0 200 Ok\r\n"
					"Content-Type: application/octet-stream\r\n"
					"Connection: close\r\n"
					"Content-Length: %ld\r\n\r\n",
				my_state->filelen);
				
				my_state->fileok = 1;
			}
			else
			{
				sprintf(buf, "HTTP/1.0 404 Not Found\r\n"
						"Content-Type: text/plain\r\n"
						"Content-Length: %lu\r\n\r\n",
			    strlen(error404));
				my_state->fileok = 0;
			}
			
	        my_state->output_bytes = TSIOBufferWrite(my_state->resp_buffer, buf, strlen(buf));
	        my_state->write_vio = TSVConnWrite(my_state->net_vc, contp, my_state->resp_reader, strlen(buf)+my_state->filelen);
	        break;
	    }
	    case TS_EVENT_VCONN_WRITE_READY:
	    {
			if(my_state->fileok != 1)
			{
				my_state->output_bytes += TSIOBufferWrite(my_state->resp_buffer, error404, strlen(error404));
				TSVIONBytesSet(my_state->write_vio, my_state->output_bytes);
				TSVIOReenable(my_state->write_vio);
	        	break;
			}
			
			if(my_state->fp != NULL)
			{
				memset(my_state->buf, 0, blocksize);
				{
					long readlen = fread(my_state->buf, 1, blocksize, my_state->fp);
					if(readlen > 0)
					{
						my_state->output_bytes += TSIOBufferWrite(my_state->resp_buffer, my_state->buf, readlen);
						my_state->already += readlen;

						if(my_state->already >= my_state->filelen)
						{
							fclose(my_state->fp);
							my_state->fp = NULL;
							TSVIONBytesSet(my_state->write_vio, my_state->output_bytes);
						}
					}
					else
					{
						fclose(my_state->fp);
						my_state->fp = NULL;
						TSVIONBytesSet(my_state->write_vio, my_state->output_bytes);
					}
				}
			}
            
	        TSVIOReenable(my_state->write_vio);
	        break;
	    }
	    case TS_EVENT_VCONN_WRITE_COMPLETE:
	            stats_cleanup(contp);
	        break;
	    case TS_EVENT_VCONN_EOS:
	    default:
	        stats_cleanup(contp);
    }
}

static int stats_dostuff(TSCont contp, TSEvent event, void *edata)
{	
	switch (event) 
	{
		case TS_EVENT_NET_ACCEPT:
		case TS_EVENT_NET_ACCEPT_FAILED:
			stats_process_accept(contp, event, (TSVConn)edata);
			break;
		case TS_EVENT_VCONN_READ_READY:
		case TS_EVENT_VCONN_READ_COMPLETE:
		case TS_EVENT_VCONN_WRITE_READY:
		case TS_EVENT_VCONN_WRITE_COMPLETE:
		case TS_EVENT_VCONN_EOS:
			stats_process_io(contp, event, edata);
			break;
		case TS_EVENT_ERROR:
		default:
			stats_cleanup(contp);
	}
    return 0;
}
