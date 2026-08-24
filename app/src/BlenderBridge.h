#pragma once

#include "TextureGenerator.h"

#include <QObject>
#include <QStringList>

class BlenderBridge : public QObject
{
public:
    explicit BlenderBridge(QObject* parent = nullptr);

    bool blenderExists() const;
    bool isMcpOnline() const;
    QString blenderPath() const;
    QStringList availableBlenderPaths() const;
    void setBlenderPath(const QString& path);

    struct ImportResult
    {
        bool ok = false;
        QString message;
        QString mode;
    };

    ImportResult importMaterial(const TextureResult& result, double bumpStrength, double normalStrength) const;

private:
    QStringList detectBlenderInstallations() const;
    QString pickDefaultBlenderPath(const QStringList& candidates) const;
    QString buildMaterialScript(const TextureResult& result, double bumpStrength, double normalStrength, bool liveSession) const;
    ImportResult importViaMcp(const QString& script) const;
    ImportResult launchBlender(const QString& script, const QString& outputDirectory) const;

    QStringList m_availablePaths;
    QString m_blenderPath;
};
