// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "toolchainmanager.h"

#include "abi.h"
#include "msvctoolchain.h"
#include "projectexplorertr.h"
#include "toolchain.h"
#include "toolchainsettingsaccessor.h"

#include <coreplugin/icore.h>

#include <utils/fileutils.h>
#include <utils/persistentsettings.h>
#include <utils/qtcassert.h>
#include <utils/algorithm.h>

#include <nanotrace/nanotrace.h>

using namespace Utils;

namespace ProjectExplorer {
namespace Internal {

// --------------------------------------------------------------------------
// ToolchainManagerPrivate
// --------------------------------------------------------------------------

struct LanguageDisplayPair
{
    Utils::Id id;
    QString displayName;
};

class ToolchainManagerPrivate
{
public:
    ~ToolchainManagerPrivate();

    std::unique_ptr<ToolchainSettingsAccessor> m_accessor;

    Toolchains m_toolChains; // prioritized List
    BadToolchains m_badToolchains;   // to be skipped when auto-detecting
    QVector<LanguageDisplayPair> m_languages;
    QList<std::pair<LanguageCategory, QString>> m_languageCategories;
    ToolchainDetectionSettings m_detectionSettings;
    bool m_loaded = false;
};

ToolchainManagerPrivate::~ToolchainManagerPrivate()
{
    qDeleteAll(m_toolChains);
    m_toolChains.clear();
}

static ToolchainManager *m_instance = nullptr;
static ToolchainManagerPrivate *d = nullptr;

} // namespace Internal

using namespace Internal;

const char DETECT_X64_AS_X32_KEY[] = "ProjectExplorer/Toolchains/DetectX64AsX32";

static Key badToolchainsKey() { return "BadToolChains"; }

// --------------------------------------------------------------------------
// ToolchainManager
// --------------------------------------------------------------------------

ToolchainManager::ToolchainManager(QObject *parent) :
    QObject(parent)
{
    Q_ASSERT(!m_instance);
    m_instance = this;

    d = new ToolchainManagerPrivate;

    connect(Core::ICore::instance(), &Core::ICore::saveSettingsRequested,
            this, &ToolchainManager::saveToolchains);
    connect(this, &ToolchainManager::toolchainsRegistered,
            this, &ToolchainManager::toolchainsChanged);
    connect(this, &ToolchainManager::toolchainsDeregistered, this, &ToolchainManager::toolchainsChanged);
    connect(this, &ToolchainManager::toolchainUpdated, this, &ToolchainManager::toolchainsChanged);

    QtcSettings * const s = Core::ICore::settings();
    d->m_detectionSettings.detectX64AsX32
        = s->value(DETECT_X64_AS_X32_KEY, ToolchainDetectionSettings().detectX64AsX32).toBool();
    d->m_badToolchains = BadToolchains::fromVariant(s->value(badToolchainsKey()));
}

ToolchainManager::~ToolchainManager()
{
    m_instance = nullptr;
    delete d;
    d = nullptr;
}

ToolchainManager *ToolchainManager::instance()
{
    return m_instance;
}

void ToolchainManager::restoreToolchains()
{
    NANOTRACE_SCOPE("ProjectExplorer", "ToolchainManager::restoreToolChains");
    QTC_ASSERT(!d->m_accessor, return);
    d->m_accessor = std::make_unique<Internal::ToolchainSettingsAccessor>();

    registerToolchains(d->m_accessor->restoreToolchains(Core::ICore::dialogParent()));

    d->m_loaded = true;
    emit m_instance->toolchainsLoaded();
}

void ToolchainManager::saveToolchains()
{
    QTC_ASSERT(d->m_accessor, return);

    d->m_accessor->saveToolchains(d->m_toolChains, Core::ICore::dialogParent());
    QtcSettings *const s = Core::ICore::settings();
    s->setValueWithDefault(DETECT_X64_AS_X32_KEY,
                           d->m_detectionSettings.detectX64AsX32,
                           ToolchainDetectionSettings().detectX64AsX32);
    s->setValue(badToolchainsKey(), d->m_badToolchains.toVariant());
}

const Toolchains &ToolchainManager::toolchains()
{
    QTC_CHECK(d->m_loaded);
    return d->m_toolChains;
}

Toolchains ToolchainManager::toolchains(const Toolchain::Predicate &predicate)
{
    QTC_ASSERT(predicate, return {});
    return Utils::filtered(d->m_toolChains, predicate);
}

Toolchain *ToolchainManager::toolchain(const Toolchain::Predicate &predicate)
{
    QTC_CHECK(d->m_loaded);
    return Utils::findOrDefault(d->m_toolChains, predicate);
}

Toolchains ToolchainManager::findToolchains(const Abi &abi)
{
    QTC_CHECK(d->m_loaded);
    Toolchains result;
    for (Toolchain *tc : std::as_const(d->m_toolChains)) {
        bool isCompatible = Utils::anyOf(tc->supportedAbis(), [abi](const Abi &supportedAbi) {
            return supportedAbi.isCompatibleWith(abi);
        });

        if (isCompatible)
            result.append(tc);
    }
    return result;
}

Toolchain *ToolchainManager::findToolchain(const QByteArray &id)
{
    QTC_CHECK(d->m_loaded);
    if (id.isEmpty())
        return nullptr;

    Toolchain *tc = Utils::findOrDefault(d->m_toolChains, Utils::equal(&Toolchain::id, id));

    // Compatibility with versions 3.5 and earlier:
    if (!tc) {
        const int pos = id.indexOf(':');
        if (pos < 0)
            return tc;

        const QByteArray shortId = id.mid(pos + 1);

        tc = Utils::findOrDefault(d->m_toolChains, Utils::equal(&Toolchain::id, shortId));
    }
    return tc;
}

bool ToolchainManager::isLoaded()
{
    return d->m_loaded;
}

void ToolchainManager::notifyAboutUpdate(Toolchain *tc)
{
    if (!tc || !d->m_toolChains.contains(tc))
        return;
    emit m_instance->toolchainUpdated(tc);
}

Toolchains ToolchainManager::registerToolchains(const Toolchains &toolchains)
{
    Toolchains registered;
    Toolchains notRegistered;

    for (Toolchain * const tc : toolchains) {
        QTC_ASSERT(tc, notRegistered << tc; continue);
        QTC_ASSERT(isLanguageSupported(tc->language()),
                   qDebug() << qPrintable("language \"" + tc->language().toString()
                                          + "\" unknown while registering \""
                                          + tc->compilerCommand().toString() + "\"");
                   notRegistered << tc;
                   continue);
        QTC_ASSERT(d->m_accessor, notRegistered << tc; continue);
        QTC_ASSERT(!d->m_toolChains.contains(tc), continue);
        QTC_ASSERT(!Utils::contains(d->m_toolChains, Utils::equal(&Toolchain::id, tc->id())),
                   notRegistered << tc;
                   continue);
        if (!tc->isAutoDetected()
            && Utils::contains(d->m_toolChains, [tc](const Toolchain *existing) {
                   return *tc == *existing;
               })) {
            notRegistered << tc;
            continue;
        }
        d->m_toolChains << tc;
        registered << tc;
    }

    if (!registered.isEmpty())
        emit m_instance->toolchainsRegistered(registered);
    return notRegistered;
}

void ToolchainManager::deregisterToolchains(const Toolchains &toolchains)
{
    QTC_CHECK(d->m_loaded);
    Toolchains deregistered;
    for (Toolchain * const tc : toolchains) {
        QTC_ASSERT(tc, continue);
        const bool removed = d->m_toolChains.removeOne(tc);
        QTC_ASSERT(removed, continue);
        deregistered << tc;
    }

    if (!deregistered.isEmpty())
        emit m_instance->toolchainsDeregistered(deregistered);
    qDeleteAll(toolchains);
}

QList<Id> ToolchainManager::allLanguages()
{
    return Utils::transform<QList>(d->m_languages, &LanguageDisplayPair::id);
}

bool ToolchainManager::registerLanguage(const Utils::Id &language, const QString &displayName)
{
    QTC_ASSERT(language.isValid(), return false);
    QTC_ASSERT(!isLanguageSupported(language), return false);
    QTC_ASSERT(!displayName.isEmpty(), return false);
    d->m_languages.push_back({language, displayName});
    return true;
}

void ToolchainManager::registerLanguageCategory(const LanguageCategory &languages, const QString &displayName)
{
    d->m_languageCategories.push_back(std::make_pair(languages, displayName));
}

QString ToolchainManager::displayNameOfLanguageId(const Utils::Id &id)
{
    QTC_ASSERT(id.isValid(), return Tr::tr("None"));
    auto entry = Utils::findOrDefault(d->m_languages, Utils::equal(&LanguageDisplayPair::id, id));
    QTC_ASSERT(entry.id.isValid(), return Tr::tr("None"));
    return entry.displayName;
}

QString ToolchainManager::displayNameOfLanguageCategory(const LanguageCategory &category)
{
    if (int(category.size()) == 1)
        return displayNameOfLanguageId(*category.begin());
    QString name = Utils::findOrDefault(d->m_languageCategories, [&category](const auto &e) {
                       return e.first == category;
                   }).second;
    QTC_ASSERT(!name.isEmpty(), return Tr::tr("None"));
    return name;
}

const QList<LanguageCategory> ToolchainManager::languageCategories()
{
    QList<LanguageCategory> categories
        = Utils::transform<QList<LanguageCategory>>(d->m_languageCategories, [](const auto &e) {
              return e.first;
          });
    const QList<Utils::Id> languages = allLanguages();
    for (const Utils::Id &l : languages) {
        if (Utils::contains(categories, [l](const LanguageCategory &lc) {
                return lc.contains(l);
            })) {
            continue;
        }
        categories.push_back({l});
    }

    return categories;
}

bool ToolchainManager::isLanguageSupported(const Utils::Id &id)
{
    return Utils::contains(d->m_languages, Utils::equal(&LanguageDisplayPair::id, id));
}

void ToolchainManager::aboutToShutdown()
{
    if (HostOsInfo::isWindowsHost())
        MsvcToolchain::cancelMsvcToolChainDetection();
}

ToolchainDetectionSettings ToolchainManager::detectionSettings()
{
    return d->m_detectionSettings;
}

void ToolchainManager::setDetectionSettings(const ToolchainDetectionSettings &settings)
{
    d->m_detectionSettings = settings;
}

void ToolchainManager::resetBadToolchains()
{
    d->m_badToolchains.toolchains.clear();
}

bool ToolchainManager::isBadToolchain(const Utils::FilePath &toolchain)
{
    return d->m_badToolchains.isBadToolchain(toolchain);
}

void ToolchainManager::addBadToolchain(const Utils::FilePath &toolchain)
{
    d->m_badToolchains.toolchains << toolchain;
}

} // namespace ProjectExplorer
