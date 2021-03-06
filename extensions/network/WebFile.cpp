#include "WebFile.h"
#include "cocos2d.h"

#include <stdio.h>

#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>
#include <list>
#include <utility>

#include <curl/curl.h>
#include <curl/easy.h>

NS_CC_EXT_BEGIN

class Caller : public cocos2d::Object
{
private:
	typedef std::pair<fn_cb_t, bool> cb_pair_t;

private:
	std::mutex _mutex;
	std::list<cb_pair_t> _queue;
    size_t _pending_requests;
    bool _scheduled;

public:
    Caller()
    : _pending_requests(0), _scheduled(false)
    {}
    
    // curl thread
	void requestCall(fn_cb_t callback, bool result)
	{
		_mutex.lock();
		_queue.push_front(cb_pair_t(callback, result));
		_mutex.unlock();
	}
    
    // main thread
    void increment()
    {
        if (++_pending_requests) {
            doScheduled();
        }
    }
    
    // main thread
    void decrement()
    {
        if (_pending_requests-- == 0) {
            doUnscheduled();
        }
    }

    // main thread communicates with curl thread via _queue
	void update(float dt)
	{
		_mutex.lock();
		while(!_queue.empty())
		{
			cb_pair_t p = _queue.back();
			p.first(p.second);
			_queue.pop_back();
            
            decrement();
		}
		_mutex.unlock();
	}

private:
    // main thread
	void doScheduled()
	{
        if (_scheduled) return;
        _scheduled = true;
		Director::getInstance()->getScheduler()->scheduleUpdateForTarget(this, 10, false);
	}

    // main thread
	void doUnscheduled()
	{
        return;
        // -_-;
		Director::getInstance()->getScheduler()->unscheduleAllForTarget(this);
	}
};

static Caller* caller = NULL;

typedef std::vector<fn_cb_t> cb_arr_t;
typedef std::map<std::string, cb_arr_t> duple_map_t;

static bool clearing = false;
static std::mutex g_mutex;
static std::vector<std::string> downloadingFiles;
static duple_map_t duplMap;

bool fileExists(const char * filename)
{
		if (FILE * file = fopen(filename, "r"))
		{
				fclose(file);
				return true;
		}
		return false;
}

size_t write(void * ptr, size_t size, size_t nmemb, void * userdata)
{
	FILE * fp = (FILE *)userdata;
	size_t written = fwrite(ptr, size, nmemb, fp);
	return written;
}

void download(std::string url, std::string destFile, fn_cb_t callback)
{
	g_mutex.lock();
	downloadingFiles.push_back(destFile);
	g_mutex.unlock();

	FILE * fp = fopen(destFile.c_str(), "wb");
	if (!fp)
	{
		CCLOGERROR("[ERROR] webfile : when open file : %s", destFile.c_str());
		caller->requestCall(callback, false);
		return;
	}

	CCLOG("download url: %s",url.c_str());
	CCLOG("dest file: %s", destFile.c_str());

	CURL * curl = curl_easy_init();
	if (curl == NULL)
	{
		CCLOG("[ERROR] webfile : cannot init curl context");
		fclose(fp);
		caller->requestCall(callback, false);
		return;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, true);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);

	CURLcode res = curl_easy_perform(curl);

	curl_easy_cleanup(curl);
	fclose(fp);

	bool succeeded = (res == 0);

	g_mutex.lock();
	duple_map_t::iterator it;
	if ( (it = duplMap.find(destFile)) != duplMap.end() &&
			!it->second.empty() )
	{
		cb_arr_t & cbArr(it->second);
		// this thread should handle all registered callback
		for(cb_arr_t::iterator i = cbArr.begin(); i != cbArr.end(); ++i)
		{
			(*i)(succeeded);
		}
		cbArr.clear();
		duplMap.erase(it);
	}

	downloadingFiles.erase(
		std::find(downloadingFiles.begin(), downloadingFiles.end(), destFile) );
	g_mutex.unlock();

	if (!succeeded)
	{
		CCLOGERROR("[ERROR] webfile : when downloading with curl : %d", res);		
		caller->requestCall(callback, false);
		return;
	}
	else caller->requestCall(callback, true);
}

void WebFile::get(const char * url, const char * destFile, fn_cb_t callback)
{
    if (caller == NULL) {
        caller = new Caller();
    }

	if(clearing)
	{
		callback(false);
		return;	
	}

	g_mutex.lock();
	if(std::find(downloadingFiles.begin(), downloadingFiles.end(), std::string(destFile))
			== downloadingFiles.end())
	{
		// not exists in current downloading list...
		if (!fileExists(destFile))
		{
            caller->increment();
            
			// file not exists -> download file
			std::thread t(&download, std::string(url), std::string(destFile), callback);
			t.detach();
		}
		else
		{
			// file exists -> finished
			callback(true);
		}	
	}
	else
	{
		// it's downloading,
		// so register our callback. and this thread's duty is over
		duplMap[std::string(destFile)].push_back(callback);
	}
	g_mutex.unlock();
}

void clear(fn_cb_t callback)
{
	caller->requestCall(callback, true);
	clearing = false;
}

void WebFile::clearStorage(fn_cb_t callback)
{
    if (caller == NULL) {
        caller = new Caller();
    }

	clearing = true;
    caller->increment();
	std::thread t(&clear, callback);
	t.detach();
}

NS_CC_EXT_END