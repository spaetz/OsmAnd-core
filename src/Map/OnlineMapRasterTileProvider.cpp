#include "OnlineMapRasterTileProvider.h"

#include <assert.h>

#include "QMainThreadTaskEvent.h"
#include "OsmAndCore/Logging.h"
#include "Concurrent.h"

#include <QCoreApplication>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>

#include <SkStream.h>
#include <SkImageDecoder.h>

OsmAnd::OnlineMapRasterTileProvider::OnlineMapRasterTileProvider(
    const QString& id_,
    const QString& urlPattern_,
    const ZoomLevel& maxZoom_ /*= 31*/,
    const ZoomLevel& minZoom_ /*= 0*/,
    const uint32_t& maxConcurrentDownloads_ /*= 1*/,
    const uint32_t& tileDimension_ /*= 256*/,
    const AlphaChannelData& alphaChannelData_ /*= AlphaChannelData::Undefined*/)
    : _processingMutex(QMutex::Recursive)
    , _currentDownloadsCount(0)
    , _localCachePath(new QDir(QDir::current()))
    , _networkAccessAllowed(true)
    , _requestsMutex(QMutex::Recursive)
    , _taskHostBridge(this)
    , localCachePath(_localCachePath)
    , networkAccessAllowed(_networkAccessAllowed)
    , id(id_)
    , urlPattern(urlPattern_)
    , minZoom(minZoom_)
    , maxZoom(maxZoom_)
    , maxConcurrentDownloads(maxConcurrentDownloads_)
    , tileDimension(tileDimension_)
    , alphaChannelData(alphaChannelData_)
{
}

OsmAnd::OnlineMapRasterTileProvider::~OnlineMapRasterTileProvider()
{
    _taskHostBridge.onOwnerIsBeingDestructed();
}

void OsmAnd::OnlineMapRasterTileProvider::setLocalCachePath( const QDir& localCachePath )
{
    QMutexLocker scopeLock(&_processingMutex);
    _localCachePath.reset(new QDir(localCachePath));
}

void OsmAnd::OnlineMapRasterTileProvider::setNetworkAccessPermission( bool allowed )
{
    _networkAccessAllowed = allowed;
}

bool OsmAnd::OnlineMapRasterTileProvider::obtainTileImmediate( const TileId& tileId, const ZoomLevel& zoom, std::shared_ptr<IMapTileProvider::Tile>& tile )
{
    // Raster tiles are not available immediately, since none of them are stored in memory unless they are just
    // downloaded. In that case, a callback will be called
    return false;
}

void OsmAnd::OnlineMapRasterTileProvider::obtainTileDeffered( const TileId& tileId, const ZoomLevel& zoom, TileReadyCallback readyCallback )
{
    assert(readyCallback != nullptr);

    {
        QMutexLocker scopeLock(&_requestsMutex);
        if(_requestedTileIds[zoom].contains(tileId))
            return;
        _requestedTileIds[zoom].insert(tileId);
    }

    Concurrent::pools->localStorage->start(new Concurrent::HostedTask(_taskHostBridge,
        [tileId, zoom, readyCallback](const Concurrent::Task* task, QEventLoop& eventLoop)
        {
            const auto pThis = reinterpret_cast<OnlineMapRasterTileProvider*>(static_cast<const Concurrent::HostedTask*>(task)->lockedOwner);

            pThis->_processingMutex.lock();

            // Check if we're already in process of downloading this tile, or
            // if this tile is in pending-download state
            if(pThis->_enqueuedTileIdsForDownload[zoom].contains(tileId) || pThis->_currentlyDownloadingTileIds[zoom].contains(tileId))
            {
                pThis->_processingMutex.unlock();
                return;
            }

            // Check if file is already in local storage
            if(pThis->_localCachePath && pThis->_localCachePath->exists())
            {
                const auto& subPath = pThis->id + QDir::separator() +
                    QString::number(zoom) + QDir::separator() +
                    QString::number(tileId.x) + QDir::separator() +
                    QString::number(tileId.y) + QString::fromLatin1(".tile");
                const auto& fullPath = pThis->_localCachePath->filePath(subPath);
                QFile tileFile(fullPath);
                if(tileFile.exists())
                {
                    // 0-sized tile means there is no data at all
                    if(tileFile.size() == 0)
                    {
                        {
                            QMutexLocker scopeLock(&pThis->_requestsMutex);
                            pThis->_requestedTileIds[zoom].remove(tileId);
                        }
                        pThis->_processingMutex.unlock();

                        std::shared_ptr<IMapTileProvider::Tile> emptyTile;
                        readyCallback(tileId, zoom, emptyTile, true);

                        return;
                    }

                    //NOTE: Here may be issue that SKIA can not handle opening files on different platforms correctly
                    auto skBitmap = new SkBitmap();
                    SkFILEStream fileStream(fullPath.toStdString().c_str());
                    if(!SkImageDecoder::DecodeStream(&fileStream, skBitmap,  SkBitmap::Config::kNo_Config, SkImageDecoder::kDecodePixels_Mode))
                    {
                        LogPrintf(LogSeverityLevel::Error, "Failed to decode tile file '%s'", fullPath.toStdString().c_str());
                        {
                            QMutexLocker scopeLock(&pThis->_requestsMutex);
                            pThis->_requestedTileIds[zoom].remove(tileId);
                        }
                        pThis->_processingMutex.unlock();

                        delete skBitmap;
                        std::shared_ptr<IMapTileProvider::Tile> emptyTile;
                        readyCallback(tileId, zoom, emptyTile, false);
                        return;
                    }

                    assert(skBitmap->width() == skBitmap->height());
                    assert(skBitmap->width() == pThis->tileDimension);

                    // Construct tile response
                    {
                        QMutexLocker scopeLock(&pThis->_requestsMutex);
                        pThis->_requestedTileIds[zoom].remove(tileId);
                    }
                    pThis->_processingMutex.unlock();
                    
                    std::shared_ptr<Tile> tile(new Tile(skBitmap, pThis->alphaChannelData));
                    readyCallback(tileId, zoom, tile, true);
                    return;
                }
            }
        
            // Well, tile is not in local cache, we need to download it
            auto tileUrl = pThis->urlPattern;
            tileUrl
                .replace(QString::fromLatin1("${zoom}"), QString::number(zoom))
                .replace(QString::fromLatin1("${x}"), QString::number(tileId.x))
                .replace(QString::fromLatin1("${y}"), QString::number(tileId.y));
            pThis->obtainTileDeffered(QUrl(tileUrl), tileId, zoom, readyCallback);
            pThis->_processingMutex.unlock();
        }));
}

void OsmAnd::OnlineMapRasterTileProvider::obtainTileDeffered( const QUrl& url, const TileId& tileId, const ZoomLevel& zoom, TileReadyCallback readyCallback )
{
    if(!_networkAccessAllowed)
        return;
    
    if(_currentDownloadsCount == maxConcurrentDownloads)
    {
        // All slots are taken, enqueue
        TileRequest tileRequest;
        tileRequest.sourceUrl = url;
        tileRequest.tileId = tileId;
        tileRequest.zoom = zoom;
        tileRequest.callback = readyCallback;

        _tileDownloadRequestsQueue.enqueue(tileRequest);
        _enqueuedTileIdsForDownload[zoom].insert(tileId);

        // Remove from requests
        {
            QMutexLocker scopeLock(&_requestsMutex);
            _requestedTileIds[zoom].remove(tileId);
        }
        
        return;
    }

    // Add to downloading and unblock processing
    _currentlyDownloadingTileIds[zoom].insert(tileId);
    _currentDownloadsCount++;
    {
        QMutexLocker scopeLock(&_requestsMutex);
        _requestedTileIds[zoom].remove(tileId);
    }
    
    Concurrent::pools->network->start(new Concurrent::HostedTask(_taskHostBridge,
        [url, tileId, zoom, readyCallback](const Concurrent::Task* task, QEventLoop& eventLoop)
        {
            const auto pThis = reinterpret_cast<OnlineMapRasterTileProvider*>(static_cast<const Concurrent::HostedTask*>(task)->lockedOwner);

            QNetworkAccessManager networkAccessManager;
            QNetworkRequest request;
            request.setUrl(url);
            request.setRawHeader("User-Agent", "OsmAnd Core");

            auto reply = networkAccessManager.get(request);
            QObject::connect(reply, &QNetworkReply::finished,
                [pThis, reply, tileId, zoom, readyCallback, &eventLoop, &networkAccessManager]()
                {
                    pThis->replyFinishedHandler(reply, tileId, zoom, readyCallback, eventLoop, networkAccessManager);
                });
            eventLoop.exec();
            return;
        }));
}

void OsmAnd::OnlineMapRasterTileProvider::replyFinishedHandler( QNetworkReply* reply, const TileId& tileId, const ZoomLevel& zoom, TileReadyCallback readyCallback, QEventLoop& eventLoop, QNetworkAccessManager& networkAccessManager )
{
    auto redirectUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if(!redirectUrl.isEmpty())
    {
        reply->deleteLater();

        QNetworkRequest newRequest;
        newRequest.setUrl(redirectUrl);
        newRequest.setRawHeader("User-Agent", "OsmAnd Core");
        auto newReply = networkAccessManager.get(newRequest);
        QObject::connect(newReply, &QNetworkReply::finished,
            [this, newReply, tileId, zoom, readyCallback, &eventLoop, &networkAccessManager]()
            {
                replyFinishedHandler(newReply, tileId, zoom, readyCallback, eventLoop, networkAccessManager);
            });
        return;
    }

    _processingMutex.lock();

    // Remove current download and process pending queue
    _currentlyDownloadingTileIds[zoom].remove(tileId);
    _currentDownloadsCount--;

#if defined(_DEBUG) || defined(DEBUG)
    LogPrintf(LogSeverityLevel::Info, "Tile downloading queue size %d", _tileDownloadRequestsQueue.size());
#endif
    while(!_tileDownloadRequestsQueue.isEmpty() && _currentDownloadsCount < maxConcurrentDownloads)
    {
        const auto& request = _tileDownloadRequestsQueue.dequeue();
        _enqueuedTileIdsForDownload[request.zoom].remove(request.tileId);

        obtainTileDeffered(request.sourceUrl, request.tileId, request.zoom, request.callback);
    }
    
    handleNetworkReply(reply, tileId, zoom, readyCallback);
    
    reply->deleteLater();
    eventLoop.exit();
}

void OsmAnd::OnlineMapRasterTileProvider::handleNetworkReply( QNetworkReply* reply, const TileId& tileId, const ZoomLevel& zoom, TileReadyCallback readyCallback )
{
    const auto& subPath = id + QDir::separator() +
        QString::number(zoom) + QDir::separator() +
        QString::number(tileId.x) + QDir::separator();
    _localCachePath->mkpath(subPath);
    const auto& fullPath = _localCachePath->filePath(subPath + QString::number(tileId.y) + QString::fromLatin1(".tile"));

    auto error = reply->error();
    if(error != QNetworkReply::NetworkError::NoError)
    {
        const auto httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        LogPrintf(LogSeverityLevel::Warning, "Failed to download tile from %s (HTTP status %d)", reply->request().url().toString().toStdString().c_str(), httpStatus);

        // 404 means that this tile does not exist, so create a zero file
        if(httpStatus == 404)
        {
            if(_localCachePath && _localCachePath->exists())
            {
                // Save to a file
                QFile tileFile(fullPath);
                if(tileFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
                {
                    tileFile.close();
                }
                else
                {
                    LogPrintf(LogSeverityLevel::Error, "Failed to mark tile as non-existent with empty file '%s'", fullPath.toStdString().c_str());
                }
            }
        }
        
        _processingMutex.unlock();
        return;
    }

#if defined(_DEBUG) || defined(DEBUG)
    LogPrintf(LogSeverityLevel::Info, "Downloaded tile from %s", reply->request().url().toString().toStdString().c_str());
#endif
    const auto& data = reply->readAll();

    if(_localCachePath && _localCachePath->exists())
    {
        // Save to a file
        QFile tileFile(fullPath);
        if(tileFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            tileFile.write(data);
            tileFile.close();

#if defined(_DEBUG) || defined(DEBUG)
            LogPrintf(LogSeverityLevel::Info, "Saved tile from %s to %s", reply->request().url().toString().toStdString().c_str(), fullPath.toStdString().c_str());
#endif
        }
        else
        {
            LogPrintf(LogSeverityLevel::Error, "Failed to save tile to '%s'", fullPath.toStdString().c_str());
        }
    }
    _processingMutex.unlock();

    // Decode in-memory if we have receiver
    if(readyCallback)
    {
        auto skBitmap = new SkBitmap();
        if(!SkImageDecoder::DecodeMemory(data.data(), data.size(), skBitmap, SkBitmap::Config::kNo_Config, SkImageDecoder::kDecodePixels_Mode))
        {
            LogPrintf(LogSeverityLevel::Error, "Failed to decode tile file from '%s'", reply->request().url().toString().toStdString().c_str());

            delete skBitmap;
            std::shared_ptr<IMapTileProvider::Tile> emptyTile;
            readyCallback(tileId, zoom, emptyTile, false);
            return;
        }

        assert(skBitmap->width() == skBitmap->height());
        assert(skBitmap->width() == tileDimension);

        std::shared_ptr<Tile> tile(new Tile(skBitmap, alphaChannelData));
        readyCallback(tileId, zoom, tile, true);
    }
}

float OsmAnd::OnlineMapRasterTileProvider::getTileDensity() const
{
    // Online tile providers do not have any idea about our tile density
    return 1.0f;
}

uint32_t OsmAnd::OnlineMapRasterTileProvider::getTileSize() const
{
    return tileDimension;
}

std::shared_ptr<OsmAnd::IMapBitmapTileProvider> OsmAnd::OnlineMapRasterTileProvider::createMapnikProvider()
{
    auto provider = new OsmAnd::OnlineMapRasterTileProvider(
        "mapnik", "http://mapnik.osmand.net/${zoom}/${x}/${y}.png",
        ZoomLevel0, ZoomLevel18, 2,
        256, AlphaChannelData::NotPresent);
    return std::shared_ptr<OsmAnd::IMapBitmapTileProvider>(static_cast<OsmAnd::IMapBitmapTileProvider*>(provider));
}

std::shared_ptr<OsmAnd::IMapBitmapTileProvider> OsmAnd::OnlineMapRasterTileProvider::createCycleMapProvider()
{
    auto provider = new OsmAnd::OnlineMapRasterTileProvider(
        "cyclemap", "http://b.tile.opencyclemap.org/cycle/${zoom}/${x}/${y}.png",
        ZoomLevel0, ZoomLevel18, 2,
        256, AlphaChannelData::NotPresent);
    return std::shared_ptr<OsmAnd::IMapBitmapTileProvider>(static_cast<OsmAnd::IMapBitmapTileProvider*>(provider));
}
