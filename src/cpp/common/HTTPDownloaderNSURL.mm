// HTTPDownloaderNSURL.mm — NSURLSession-backed HTTPDownloader for the Apple targets
// SPDX-License-Identifier: GPL-3.0+
// See HTTPDownloaderNSURL.h for why this exists.

#include "common/HTTPDownloaderNSURL.h"

#include "common/Console.h"
#include "common/Timer.h"

#import <Foundation/Foundation.h>

#include <cstring>

// NSURLSession is already asynchronous, so a request needs no polling of its own: the
// completion handler fills the request in and publishes it by storing State::Complete last.
// The base class reads `state` (an atomic) before touching data, so that store is the
// happens-before edge that makes the rest of the fields safe to read.
struct HTTPDownloaderNSURL::Request : HTTPDownloader::Request
{
	NSURLSessionDataTask* task = nil;
};

HTTPDownloaderNSURL::HTTPDownloaderNSURL() = default;

HTTPDownloaderNSURL::~HTTPDownloaderNSURL()
{
	if (m_session)
	{
		NSURLSession* session = (__bridge_transfer NSURLSession*)m_session;
		[session invalidateAndCancel];
		m_session = nullptr;
	}
}

bool HTTPDownloaderNSURL::Initialize(std::string user_agent)
{
	NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
	config.HTTPAdditionalHeaders = @{@"User-Agent" : [NSString stringWithUTF8String:user_agent.c_str()]};
	config.timeoutIntervalForRequest = m_timeout;
	config.HTTPMaximumConnectionsPerHost = static_cast<NSInteger>(m_max_active_requests);
	// Covers and achievement art are re-fetched deliberately; a disk cache would only add a
	// second copy of data the caller already stores itself.
	config.URLCache = nil;
	config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;

	NSURLSession* session = [NSURLSession sessionWithConfiguration:config];
	if (!session)
	{
		Console.Error("NSURLSession: could not create session");
		return false;
	}

	m_session = (__bridge_retained void*)session;
	return true;
}

HTTPDownloader::Request* HTTPDownloaderNSURL::InternalCreateRequest()
{
	return new Request();
}

void HTTPDownloaderNSURL::InternalPollRequests()
{
	// Nothing to do — NSURLSession drives its own I/O and publishes through the completion
	// handler. The base class still walks the request list for timeouts and progress.
}

bool HTTPDownloaderNSURL::StartRequest(HTTPDownloader::Request* request)
{
	Request* req = static_cast<Request*>(request);

	NSString* url_string = [NSString stringWithUTF8String:req->url.c_str()];
	NSURL* url = url_string ? [NSURL URLWithString:url_string] : nil;
	if (!url)
	{
		Console.Error("NSURLSession: malformed URL '%s'", req->url.c_str());
		req->state.store(Request::State::Complete);
		req->status_code = HTTP_STATUS_ERROR;
		return false;
	}

	NSMutableURLRequest* nsreq = [NSMutableURLRequest requestWithURL:url];
	if (req->type == Request::Type::Post)
	{
		nsreq.HTTPMethod = @"POST";
		nsreq.HTTPBody = [NSData dataWithBytes:req->post_data.data() length:req->post_data.size()];
		[nsreq setValue:@"application/x-www-form-urlencoded" forHTTPHeaderField:@"Content-Type"];
	}

	req->start_time = Common::Timer::GetCurrentValue();
	req->state.store(Request::State::Started);

	NSURLSession* session = (__bridge NSURLSession*)m_session;
	NSURLSessionDataTask* task = [session dataTaskWithRequest:nsreq
		completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
			// The base class may already have given up on this request and cancelled it; in
			// that case it owns the teardown and must not be raced.
			if (req->state.load() == Request::State::Cancelled)
				return;

			if (error)
			{
				Console.Error("NSURLSession: request for '%s' failed: %s", req->url.c_str(),
					[[error localizedDescription] UTF8String]);
				req->status_code = HTTP_STATUS_ERROR;
			}
			else
			{
				NSHTTPURLResponse* http = [response isKindOfClass:[NSHTTPURLResponse class]] ?
					(NSHTTPURLResponse*)response : nil;
				req->status_code = http ? static_cast<s32>(http.statusCode) : HTTP_STATUS_ERROR;

				NSString* content_type = http ? [http valueForHTTPHeaderField:@"Content-Type"] : nil;
				if (content_type)
					req->content_type = [content_type UTF8String];

				if (data.length > 0)
				{
					req->data.resize(data.length);
					std::memcpy(req->data.data(), data.bytes, data.length);
				}
				req->content_length = static_cast<u32>(req->data.size());
			}

			// Publish last: everything above must be visible before the base class sees Complete.
			req->state.store(Request::State::Complete);
		}];

	if (!task)
	{
		Console.Error("NSURLSession: could not create task for '%s'", req->url.c_str());
		req->status_code = HTTP_STATUS_ERROR;
		req->state.store(Request::State::Complete);
		return false;
	}

	req->task = task;
	[task resume];
	return true;
}

void HTTPDownloaderNSURL::CloseRequest(HTTPDownloader::Request* request)
{
	Request* req = static_cast<Request*>(request);
	if (req->task)
	{
		[req->task cancel];
		req->task = nil;
	}
	delete req;
}

std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(std::string user_agent)
{
	std::unique_ptr<HTTPDownloaderNSURL> instance(new HTTPDownloaderNSURL());
	if (!instance->Initialize(std::move(user_agent)))
		return {};
	return instance;
}
