/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Sonic Lineup
    Comparative visualisation and alignment of related audio recordings
    Centre for Digital Music, Queen Mary, University of London.
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#include "MainWindow.h"

#include "system/System.h"
#include "system/Init.h"
#include "base/TempDirectory.h"
#include "base/PropertyContainer.h"
#include "base/Preferences.h"
#include "data/fileio/PlaylistFileReader.h"
#include "widgets/TipDialog.h"
#include "svcore/plugin/PluginScan.h"

#include <QMetaType>
#include <QApplication>
#include <QScreen>
#include <QMessageBox>
#include <QTranslator>
#include <QLocale>
#include <QSettings>
#include <QIcon>
#include <QSessionManager>
#include <QDir>

#include <iostream>
#include <signal.h>

#include "../version.h"

#include <vamp-hostsdk/PluginHostAdapter.h>

static QMutex cleanupMutex;
static bool cleanedUp = false;

static void
signalHandler(int /* signal */)
{
    // Avoid this happening more than once across threads

    cerr << "signalHandler: cleaning up and exiting" << endl;

    if (cleanupMutex.tryLock(5000)) {
        if (!cleanedUp) {
            TempDirectory::getInstance()->cleanup();
            cleanedUp = true;
        }
        cleanupMutex.unlock();
    }
    
    exit(0);
}

class VectApplication : public QApplication
{
public:
    VectApplication(int &argc, char **argv) :
        QApplication(argc, argv),
        m_mainWindow(0) { }
    virtual ~VectApplication() { }

    void setMainWindow(MainWindow *mw) { m_mainWindow = mw; }
    void releaseMainWindow() { m_mainWindow = 0; }

    virtual void commitData(QSessionManager &manager) {
        if (!m_mainWindow) return;
        bool mayAskUser = manager.allowsInteraction();
        bool success = m_mainWindow->commitData(mayAskUser);
        manager.release();
        if (!success) manager.cancel();
    }

protected:
    MainWindow *m_mainWindow;
};

static QString
getEnvQStr(QString variable)
{
#ifdef Q_OS_WIN32
    std::wstring wvar = variable.toStdWString();
    wchar_t *value = _wgetenv(wvar.c_str());
    if (!value) return QString();
    else return QString::fromStdWString(std::wstring(value));
#else
    std::string var = variable.toStdString();
    return QString::fromUtf8(qgetenv(var.c_str()));
#endif
}

static void
putEnvQStr(QString assignment)
{
#ifdef Q_OS_WIN32
    std::wstring wassignment = assignment.toStdWString();
    _wputenv(_wcsdup(wassignment.c_str()));
#else
    putenv(strdup(assignment.toUtf8().data()));
#endif
}

static void
setupMyVampPath()
{
    // This based on similar logic from the Tony application
    
    QString myVampPath = getEnvQStr("SONIC_LINEUP_VAMP_PATH");

#ifdef Q_OS_WIN32
    QChar sep(';');
#else
    QChar sep(':');
#endif
    
    if (myVampPath == "") {

        QString appName = QApplication::applicationName();
        QString myDir = QApplication::applicationDirPath();
        QString binaryName = QFileInfo(QCoreApplication::arguments().at(0))
            .fileName();

#ifdef Q_OS_WIN32
        QString programFiles = getEnvQStr("ProgramFiles");
        if (programFiles == "") programFiles = "C:\\Program Files";
        QString pfPath(programFiles + "\\" + appName);
        myVampPath = myDir + sep + pfPath;
#else
#ifdef Q_OS_MAC
        myVampPath = myDir + "/../Resources";
        (void)sep; // unused
#else
        if (binaryName != "") {
            myVampPath =
                myDir + "/../lib/" + binaryName + sep;
        }
        myVampPath = myVampPath +
            myDir + "/../lib/" + appName + sep +
            myDir;
#endif
#endif
    }

    SVCERR << "Setting VAMP_PATH to " << myVampPath
           << " for Sonic Lineup plugins" << endl;

    QString env = "VAMP_PATH=" + myVampPath;

    // Windows lacks setenv, must use putenv (different arg convention)
    putEnvQStr(env);
}

int
main(int argc, char **argv)
{
    if (argc == 2 && (QString(argv[1]) == "--version" ||
                      QString(argv[1]) == "-v")) {
        cerr << VECT_VERSION << endl;
        exit(0);
    }

    svSystemSpecificInitialisation();

    VectApplication application(argc, argv);

    QApplication::setOrganizationName("sonic-visualiser");
    QApplication::setOrganizationDomain("sonicvisualiser.org");
    QApplication::setApplicationName("Sonic Lineup");

    setupMyVampPath();

    QStringList args = application.arguments();

    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);

#ifndef Q_OS_WIN32
    signal(SIGHUP,  signalHandler);
    signal(SIGQUIT, signalHandler);
#endif

    svSystemSpecificInitialisation();

    bool audioOutput = true;

    if (args.contains("--help") || args.contains("-h") || args.contains("-?")) {
        std::cerr << QApplication::tr(
            "\nSonic Lineup is a comparative viewer for sets of related audio recordings.\n\nUsage:\n\n  %1 [--no-audio] [<file1>, <file2>...]\n\n  --no-audio: Do not attempt to open an audio output device\n  <file1>, <file2>...: Audio files; Sonic Lineup is designed for comparative\nviewing of multiple recordings of the same music or other related material.\n").arg(argv[0]).toStdString() << std::endl;
        exit(2);
    }

    if (args.contains("--no-audio")) {
        audioOutput = false;
    }

    if (args.contains("--first-run")) {
        QSettings settings;
        settings.clear();
    }

    InteractiveFileFinder::getInstance()->setApplicationSessionExtension("vect");

    QSettings settings;
    settings.beginGroup("Preferences");
    // Running plugins in-process allows us to use the serialise
    // option in the MATCH plugin, which cuts down on memory pressure
    // and makes things go more smoothly
    settings.setValue("run-vamp-plugins-in-process", true);
    settings.endGroup();

    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication::setAttribute(Qt::AA_DontShowIconsInMenus);

    QIcon icon;
    int sizes[] = { 16, 22, 24, 32, 48, 64, 128 };
    for (int i = 0; i < (int)(sizeof(sizes)/sizeof(sizes[0])); ++i) {
        icon.addFile(QString(":icons/sv-%1x%2.png").arg(sizes[i]).arg(sizes[i]));
    }
    QApplication::setWindowIcon(icon);

    QString language = QLocale::system().name();

    QTranslator qtTranslator;
    QString qtTrName = QString("qt_%1").arg(language);
    SVCERR << "Loading " << qtTrName.toStdString() << "..." << endl;
    bool success = false;
    if (!(success = qtTranslator.load(QLocale(), qtTrName))) {
        QString qtDir = getenv("QTDIR");
        if (qtDir != "") {
            success = qtTranslator.load
                (QLocale(), qtTrName, QDir(qtDir).filePath("translations"));
        }
    }
    if (!success) {
        SVCERR << "Failed to load Qt translation for locale" << endl;
    }
    application.installTranslator(&qtTranslator);

    QTranslator svecTranslator;
    QString svecTrName = QString("vect_%1").arg(language);
    SVCERR << "Loading " << svecTrName << "..." << endl;
    if (svecTranslator.load(svecTrName, ":i18n")) {
        application.installTranslator(&svecTranslator);
    } else {
        SVCERR << "Failed to load translation" << endl;
    }

    StoreStartupLocale();

    // Make known-plugins query as early as possible
    PluginScan::getInstance()->scan();
    
    // Permit size_t and PropertyName to be used as args in queued signal calls
    qRegisterMetaType<PropertyContainer::PropertyName>("PropertyContainer::PropertyName");

    MainWindow::AudioMode audioMode = 
        MainWindow::AUDIO_PLAYBACK_NOW_RECORD_LATER;

    if (!audioOutput) {
        audioMode = MainWindow::AUDIO_NONE;
    } 
    
    MainWindow *gui = new MainWindow(audioMode);
    application.setMainWindow(gui);

    QScreen *screen = QApplication::primaryScreen();
    QRect available = screen->availableGeometry();

    int width = available.width() * 2 / 3;
    int height = available.height() / 2;
    if (height < 450) height = available.height() * 2 / 3;
    if (width > height * 2) width = height * 2;

    settings.beginGroup("MainWindow");

    QSize size = settings.value("size", QSize(width, height)).toSize();
    gui->resizeConstrained(size);

    if (settings.contains("position")) {
        QRect prevrect(settings.value("position").toPoint(), size);
        if (!(available & prevrect).isEmpty()) {
            gui->move(prevrect.topLeft());
        }
    }

    if (settings.value("maximised", false).toBool()) {
        gui->setWindowState(Qt::WindowMaximized);
    }

    settings.endGroup();
    
    gui->show();

    SmallSession session;
    bool haveSession = false;

    QStringList filePaths;
    
    for (QStringList::iterator i = args.begin(); i != args.end(); ++i) {

        if (i == args.begin()) continue;
        if (i->startsWith('-')) continue;

        QString arg = *i;

        // If an arg is a playlist file, we can streamline things and
        // make sure we get the proper absolute paths by expanding it
        // here, rather than adding it to the session and waiting for
        // it to be expanded in the main application logic. (That
        // would work too, it's just not so clean a user experience.)

        if (PlaylistFileReader::isSupported(arg)) {
            PlaylistFileReader reader(arg);
            if (!reader.isOK()) {
                // But if we can't open the playlist file, add it to
                // the session as if it were just any old file and let
                // the main application worry about it later - we
                // don't want to be popping up dialogs before the app
                // has been exec'd
                filePaths.push_back(arg);
            } else {
                auto playlist = reader.load();
                for (auto entry: playlist) {
                    filePaths.push_back(entry);
                }
            }
        } else {
            filePaths.push_back(arg);
        }
    }

    for (auto filePath: filePaths) {

        // Add the argument to our session as a file path or URL to be
        // opened. We want to avoid relative file paths, but to do so
        // we must first check that they are not absolute URLs.
        
        QUrl url(filePath);
        if (url.isRelative()) {
            filePath = QFileInfo(filePath).absoluteFilePath();
        }
        
        if (session.mainFile == "") {
            session.mainFile = filePath;
        } else {
            session.additionalFiles.push_back(filePath);
        }

        haveSession = true;
    }

    if (haveSession) {
        gui->openSmallSession(session);
    } else if (!gui->reopenLastSession()) {
        QTimer::singleShot(400, gui, SLOT(introDialog()));
    } else {
        // Do this here only if not showing the intro dialog -
        // otherwise the introDialog function will do this after it
        // has shown the dialog, so we don't end up with both at once
        gui->checkForNewerVersion();
    }

    int rv = application.exec();

    cleanupMutex.lock();
    TempDirectory::getInstance()->cleanup();
    application.releaseMainWindow();

    delete gui;

    cleanupMutex.unlock();

    return rv;
}
