/***************************************************************************
 * QGeoView is a Qt / C ++ widget for visualizing geographic data.
 * Copyright (C) 2018-2024 Andrey Yaroshenko.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see https://www.gnu.org/licenses.
 ****************************************************************************/

#include "QGVLayerTilesOnline.h"
#include "Raster/QGVImage.h"

#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>
#include <QtConcurrent/QtConcurrent>
#include <QDir>

QGVLayerTilesOnline::~QGVLayerTilesOnline()
{
    qDeleteAll(mRequest);
}

void QGVLayerTilesOnline::request(const QGV::GeoTilePos& tilePos)
{
    Q_ASSERT(QGV::getNetworkManager());

    const QUrl url(tilePosToUrl(tilePos));

    QNetworkRequest request(url);
    QSslConfiguration conf = request.sslConfiguration();
    conf.setPeerVerifyMode(QSslSocket::VerifyNone);

    request.setSslConfiguration(conf);
    request.setRawHeader("User-Agent",
                         "Mozilla/5.0 (Windows; U; MSIE "
                         "6.0; Windows NT 5.1; SV1; .NET "
                         "CLR 2.0.50727)");
    request.setAttribute(QNetworkRequest::HttpPipeliningAllowedAttribute, true);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);

    QNetworkReply* reply = QGV::getNetworkManager()->get(request);

    mRequest[tilePos] = reply;
    connect(reply, &QNetworkReply::finished, reply, [this, reply, tilePos]() { onReplyFinished(reply, tilePos); });

    qgvDebug() << "request" << url;
}

void QGVLayerTilesOnline::cancel(const QGV::GeoTilePos& tilePos)
{
    removeReply(tilePos);
}

void QGVLayerTilesOnline::onReplyFinished(QNetworkReply* reply, const QGV::GeoTilePos& tilePos)
{
    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() != QNetworkReply::OperationCanceledError) {
            qgvCritical() << "ERROR" << reply->errorString();
        }
        removeReply(tilePos);
        if (loadTileFromCache(tilePos).isEmpty()) {
            return;
        } else {
            const auto rawImage = loadTileFromCache(tilePos);
            auto tile = new QGVImage();
            tile->setGeometry(tilePos.toGeoRect());
            tile->loadImage(rawImage);
            cacheTile(rawImage, tilePos);
            onTile(tilePos, tile);
            return;
        }
    }
    const auto rawImage = reply->readAll();
    auto tile = new QGVImage();
    tile->setGeometry(tilePos.toGeoRect());
    tile->loadImage(rawImage);
    tile->setProperty("drawDebug",
                      QString("%1\ntile(%2,%3,%4)")
                              .arg(reply->url().toString())
                              .arg(tilePos.zoom())
                              .arg(tilePos.pos().x())
                              .arg(tilePos.pos().y()));
    removeReply(tilePos);
    cacheTile(rawImage, tilePos);
    onTile(tilePos, tile);
}

void QGVLayerTilesOnline::removeReply(const QGV::GeoTilePos& tilePos)
{
    QNetworkReply* reply = mRequest.value(tilePos, nullptr);
    if (reply == nullptr) {
        return;
    }
    mRequest.remove(tilePos);
    reply->abort();
    reply->close();
    reply->deleteLater();
}

void QGVLayerTilesOnline::initDatabase()
{
    mDb = QSqlDatabase::addDatabase("QSQLITE", getName());
    mDb.setDatabaseName(QDir::currentPath() + "/" + getName() + ".db");
    mDb.open();

    QString createTableQuery = R"(CREATE TABLE IF NOT EXISTS Tiles (
                                          zoom INTEGER,
                                          pos_x INTEGER,
                                          pos_y INTEGER,
                                          data BLOB,
                                          PRIMARY KEY (zoom, pos_x, pos_y)
                                        ))";
    QSqlQuery createQuery(mDb);
    if (!createQuery.exec(createTableQuery)) {
        qDebug() << "Failed to create table: " << createQuery.lastError();
        return;
    }
}

void QGVLayerTilesOnline::cacheTile(const QByteArray &rawData, const QGV::GeoTilePos& tilePos)
{
    if (!mDb.isOpen()) {
        initDatabase();
    }

    QSqlQuery query(mDb);
    query.prepare("INSERT OR IGNORE INTO Tiles (zoom, pos_x, pos_y, data) VALUES (?, ?, ?, ?)");
    query.addBindValue(tilePos.zoom());
    query.addBindValue(tilePos.pos().x());
    query.addBindValue(tilePos.pos().y());
    query.addBindValue(rawData);

    if (!query.exec()) {
        qDebug() << "Failed to cache tile: " << query.lastError();
    }
}

const QByteArray QGVLayerTilesOnline::loadTileFromCache(const QGV::GeoTilePos& tilePos)
{
    if (!mDb.isOpen()) {
        initDatabase();
    }

    QSqlQuery query(mDb);
    query.prepare("SELECT data FROM Tiles WHERE zoom = ? AND pos_x = ? AND pos_y = ?");
    query.addBindValue(tilePos.zoom());
    query.addBindValue(tilePos.pos().x());
    query.addBindValue(tilePos.pos().y());

    if (query.exec() && query.next()) {
        return query.value(0).toByteArray();
    } else {
        return QByteArray();
    }
}
