// HTTPDownloaderNSURL.h — NSURLSession-backed HTTPDownloader for the Apple targets
// SPDX-License-Identifier: GPL-3.0+
//
// [F5] HTTPDownloader::Create returned nullptr on both Apple targets because the libcurl
// backend is excluded from those builds (common/CMakeLists.txt). With no transport at all,
// GameList::DownloadCovers bailed immediately and Achievements could not reach the server —
// RetroAchievements and automatic cover art were both dead at the transport layer.
//
// NSURLSession is part of the base system, so this adds no third-party dependency and no
// new App Store review surface, unlike vendoring libcurl for iOS.

#pragma once

#include "common/HTTPDownloader.h"

#include <string>

class HTTPDownloaderNSURL final : public HTTPDownloader
{
public:
	HTTPDownloaderNSURL();
	~HTTPDownloaderNSURL() override;

	bool Initialize(std::string user_agent);

protected:
	Request* InternalCreateRequest() override;
	void InternalPollRequests() override;
	bool StartRequest(HTTPDownloader::Request* request) override;
	void CloseRequest(HTTPDownloader::Request* request) override;

private:
	struct Request;

	void* m_session = nullptr; // NSURLSession, retained
};
