/*
 *  Copyright (C) 2019 Aleix Pol Gonzalez <aleixpol@kde.org>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License as
 *  published by the Free Software Foundation; either version 2 of
 *  the License or (at your option) version 3 or any later version
 *  accepted by the membership of KDE e.V. (or its successor approved
 *  by the membership of KDE e.V.), which shall act as a proxy
 *  defined in Section 14 of version 3 of the license.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QApplication>
#include <QLocale>
#include <QByteArray>
#include <QStandardPaths>
#include <QAbstractListModel>
#include <QVector>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QSortFilterProxyModel>
#include <QQuickImageProvider>
#include <QCommandLineParser>
#include <QFontMetrics>
#include <QPainter>
#include <QClipboard>
#include <QQuickWindow>
#include <KLocalizedString>
#include <KAboutData>
#include <KQuickAddons/QtQuickSettings>
#include <KCrash>
#include <KDBusService>
#include <KWindowConfig>
#include <QDebug>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusConnectionInterface>
#include <QSessionManager>

#include "emojiersettings.h"
#include "config-workspace.h"

#undef signals
#include <ibus.h>

struct Emoji {
    QString content;
    QString description;
    QString category;
};

class TextImageProvider : public QQuickImageProvider
{
public:
    TextImageProvider()
        : QQuickImageProvider(QQuickImageProvider::Pixmap)
    {
    }

    QPixmap requestPixmap(const QString &id, QSize *_size, const QSize &requestedSize) override
    {
        QPixmap dummy;

        const QString renderString = id.mid(1); //drop initial /

        QSize size = requestedSize;
        QFont font;
        if (!size.isValid()) {
            QFontMetrics fm(font, &dummy);
            size = { fm.horizontalAdvance(renderString), fm.height() };
        } else {
            font.setPointSize((requestedSize.height() * 3) / 4);
        }
        if (_size) {
            *_size = size;
        }

        QPixmap pixmap(size.width(), size.height());
        pixmap.fill(Qt::transparent);
        QPainter p;
        p.begin(&pixmap);
        p.setFont(font);
        p.drawText(QRect(0, 0, size.width(), size.height()), Qt::AlignCenter, renderString);
        p.end();
        return pixmap;
    }
};

class AbstractEmojiModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum EmojiRole { CategoryRole = Qt::UserRole + 1 };

    int rowCount(const QModelIndex & parent = {}) const override { return parent.isValid() ? 0 : m_emoji.count(); }
    QVariant data(const QModelIndex & index, int role) const override {
        if (!checkIndex(index, QAbstractItemModel::CheckIndexOption::IndexIsValid | QAbstractItemModel::CheckIndexOption::ParentIsInvalid | QAbstractItemModel::CheckIndexOption::DoNotUseParent)  || index.column() != 0)
            return {};

        const auto &emoji = m_emoji[index.row()];
        switch(role) {
            case Qt::DisplayRole:
                return emoji.content;
            case Qt::ToolTipRole:
                return emoji.description;
            case CategoryRole:
                return emoji.category;
        }
        return {};
    }

protected:
    QVector<Emoji> m_emoji;
};

class EmojiModel : public AbstractEmojiModel
{
    Q_OBJECT
    Q_PROPERTY(QStringList categories MEMBER m_categories CONSTANT)
public:
    enum EmojiRole { CategoryRole = Qt::UserRole + 1 };

    EmojiModel() {
        QLocale locale;
        const auto bcp = locale.bcp47Name();
        const QString dictName = "ibus/dicts/emoji-" + QString(bcp).replace(QLatin1Char('-'), QLatin1Char('_')) + ".dict";
        const QString path = QStandardPaths::locate(QStandardPaths::GenericDataLocation, dictName);
        if (path.isEmpty()) {
            qWarning() << "could not find" << dictName;
            return;
        }

        QVector<QString> dicts = {path};
        const auto idxSpecific = bcp.indexOf(QLatin1Char('-'));
        if (idxSpecific > 0) {
            const QString genericDictName = "ibus/dicts/emoji-" + bcp.left(idxSpecific) + ".dict";
            const QString genericPath = QStandardPaths::locate(QStandardPaths::GenericDataLocation, genericDictName);

            if (!genericPath.isEmpty()) {
                dicts << genericPath;
            }
        }

        QSet<QString> categories;
        QSet<QString> processedEmoji;
        for (const auto &dictPath : qAsConst(dicts)) {
            GSList *list = ibus_emoji_data_load (dictPath.toUtf8().constData());
            m_emoji.reserve(g_slist_length(list));
            for (GSList *l = list; l; l = l->next) {
                IBusEmojiData *data = (IBusEmojiData *) l->data;
                if (!IBUS_IS_EMOJI_DATA (data)) {
                    qWarning() << "Your dict format is no longer supported.\n"
                                "Need to create the dictionaries again.";
                    g_slist_free (list);
                    return;
                }

                const QString emoji = QString::fromUtf8(ibus_emoji_data_get_emoji(data));
                const QString description = ibus_emoji_data_get_description(data);
                if (description == QString::fromUtf8("↑↑↑") || description.isEmpty() || processedEmoji.contains(emoji)) {
                    continue;
                }

                const QString category = QString::fromUtf8(ibus_emoji_data_get_category(data));
                categories.insert(category);
                m_emoji += { emoji, description, category };
                processedEmoji << emoji;
            }
            g_slist_free (list);
        }

        categories.remove({});
        m_categories = categories.values();
        m_categories.sort();
        m_categories.prepend({});
        m_categories.prepend(QStringLiteral(":find:"));
        m_categories.prepend(QStringLiteral(":recent:"));
    }

    Q_SCRIPTABLE QString findFirstEmojiForCategory(const QString &category) {
        for (const Emoji &emoji : m_emoji) {
            if (emoji.category == category)
                return emoji.content;
        }
        return {};
    }

private:
    QStringList m_categories;
};

class RecentEmojiModel : public AbstractEmojiModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount CONSTANT)
public:
    RecentEmojiModel()
    {
        refresh();
    }

    Q_SCRIPTABLE void includeRecent(const QString &emoji, const QString &emojiDescription) {
        QStringList recent = m_settings.recent();
        QStringList recentDescriptions = m_settings.recentDescriptions();

        const int idx = recent.indexOf(emoji);
        if (idx >= 0) {
            recent.removeAt(idx);
            recentDescriptions.removeAt(idx);
        }
        recent.prepend(emoji);
        recent = recent.mid(0, 50);
        m_settings.setRecent(recent);

        recentDescriptions.prepend(emojiDescription);
        recentDescriptions = recentDescriptions.mid(0, 50);
        m_settings.setRecentDescriptions(recentDescriptions);
        m_settings.save();

        refresh();
    }

private:
    void refresh()
    {
        beginResetModel();
        auto recent = m_settings.recent();
        auto recentDescriptions = m_settings.recentDescriptions();
        int i = 0;
        m_emoji.clear();
        for (const QString &c : recent) {
            m_emoji += { c, recentDescriptions.at(i++), QString{} };
        }
        endResetModel();
    }

    EmojierSettings m_settings;
};

class CategoryModelFilter : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(QString category READ category WRITE setCategory)
public:
    QString category() const { return m_category; }
    void setCategory(const QString &category) {
        if (m_category != category) {
            m_category = category;
            invalidateFilter();
        }
    }

    bool filterAcceptsRow(int source_row, const QModelIndex & source_parent) const override {
        return m_category.isEmpty() || sourceModel()->index(source_row, 0, source_parent).data(EmojiModel::CategoryRole).toString() == m_category;
    }

private:
    QString m_category;
};

class SearchModelFilter : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(QString search READ search WRITE setSearch)
public:
    QString search() const { return m_search; }
    void setSearch(const QString &search) {
        if (m_search != search) {
            m_search = search;
            invalidateFilter();
        }
    }

    bool filterAcceptsRow(int source_row, const QModelIndex & source_parent) const override {
        return sourceModel()->index(source_row, 0, source_parent).data(Qt::ToolTipRole).toString().contains(m_search, Qt::CaseInsensitive);
    }

private:
    QString m_search;
};

class CopyHelperPrivate : public QObject
{
    Q_OBJECT
    public:
        Q_INVOKABLE static void copyTextToClipboard(const QString& text)
        {
            QClipboard *clipboard = qGuiApp->clipboard();
            clipboard->setText(text, QClipboard::Clipboard);
            clipboard->setText(text, QClipboard::Selection);
        }
};

class EngineWatcher : public QObject
{
public:
    EngineWatcher(QQmlApplicationEngine* engine)
        : QObject(engine)
    {
        connect(engine, &QQmlApplicationEngine::objectCreated, this, &EngineWatcher::integrateObject);
    }

    void integrateObject(QObject* object) {
        QWindow* window = qobject_cast<QWindow*>(object);

        auto conf = KSharedConfig::openConfig();
        KWindowConfig::restoreWindowSize(window, conf->group("Window"));

        object->installEventFilter(this);
    }

    bool eventFilter(QObject * object, QEvent * event)
    {
        if (event->type() == QEvent::Close) {
            QWindow* window = qobject_cast<QWindow*>(object);

            auto conf = KSharedConfig::openConfig();
            auto group = conf->group("Window");
            KWindowConfig::saveWindowSize(window, group);
            group.sync();
        }
        return false;
    }
};

int main(int argc, char** argv)
{
    QGuiApplication::setFallbackSessionManagementEnabled(false);
    QApplication app(argc, argv);
    app.setAttribute(Qt::AA_UseHighDpiPixmaps, true);
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("preferences-desktop-emoticons")));
    KCrash::initialize();
    KQuickAddons::QtQuickSettings::init();

    KLocalizedString::setApplicationDomain("org.kde.plasma.emojier");

    KAboutData about(QStringLiteral("org.kde.plasma.emojier"), i18n("Emoji Picker"), QStringLiteral(WORKSPACE_VERSION_STRING), i18n("Emoji Picker"),
             KAboutLicense::GPL, i18n("(C) 2019 Aleix Pol i Gonzalez"));
    about.addAuthor( QStringLiteral("Aleix Pol i Gonzalez"), QString(), QStringLiteral("aleixpol@kde.org") );
    about.setTranslator(i18nc("NAME OF TRANSLATORS", "Your names"), i18nc("EMAIL OF TRANSLATORS", "Your emails"));
//     about.setProductName("");
    about.setProgramLogo(app.windowIcon());
    KAboutData::setApplicationData(about);

    auto disableSessionManagement = [](QSessionManager &sm) {
        sm.setRestartHint(QSessionManager::RestartNever);
    };
    QObject::connect(&app, &QGuiApplication::commitDataRequest, disableSessionManagement);
    QObject::connect(&app, &QGuiApplication::saveStateRequest, disableSessionManagement);

    KDBusService::StartupOptions startup = nullptr;
    {
        QCommandLineParser parser;

        QCommandLineOption replaceOption({QStringLiteral("replace")},
                                 i18n("Replace an existing instance"));
        parser.addOption(replaceOption);
        about.setupCommandLine(&parser);
        parser.process(app);
        about.processCommandLine(&parser);

        if (parser.isSet(replaceOption)) {
            startup |= KDBusService::Replace;
        }
    }

    KDBusService* service = new KDBusService(KDBusService::Unique | startup, &app);

    qmlRegisterType<EmojiModel>("org.kde.plasma.emoji", 1, 0, "EmojiModel");
    qmlRegisterType<CategoryModelFilter>("org.kde.plasma.emoji", 1, 0, "CategoryModelFilter");
    qmlRegisterType<SearchModelFilter>("org.kde.plasma.emoji", 1, 0, "SearchModelFilter");
    qmlRegisterType<RecentEmojiModel>("org.kde.plasma.emoji", 1, 0, "RecentEmojiModel");
    qmlRegisterSingletonType<CopyHelperPrivate>("org.kde.plasma.emoji", 1, 0, "CopyHelper", [] (QQmlEngine*, QJSEngine*) -> QObject* { return new CopyHelperPrivate; });

    QQmlApplicationEngine engine;
    new EngineWatcher(&engine);
    engine.addImageProvider(QLatin1String("text"), new TextImageProvider);
    engine.load(QUrl(QStringLiteral("qrc:/ui/emojier.qml")));

    QObject::connect(service, &KDBusService::activateRequested, &engine, [&engine](const QStringList &/*arguments*/, const QString &/*workingDirectory*/) {
        for (QObject* object : engine.rootObjects()) {
            auto w = qobject_cast<QQuickWindow*>(object);
            if (!w)
                continue;
            w->setVisible(true);
            w->raise();
        }
    });

    return app.exec();
}

#include "emojier.moc"
