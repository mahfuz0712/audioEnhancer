#include <windows.h>
#include <shellapi.h>

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QGuiApplication>
#include <QStyleHints>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "headers/audio_engine.h"
#include "version.h"

namespace {
constexpr int kTimerMs = 2500;

QString fromWide(const std::wstring& value)
{
    return QString::fromStdWString(value);
}

}

class MainWindow final : public QWidget
{
public:
    MainWindow()
    {
        setWindowTitle("Audio Enhancer");
        setWindowIcon(QIcon(":/app.ico"));
        resize(680, 520);

        loadSettings();
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        refreshDevices();
        applyTone();

        buildUi();
        buildTray();

        connect(&timer_, &QTimer::timeout, this, [this]() {
            refreshDevices();
            if (checkUpdates_ && lastUpdateCheck_ != 0 &&
                QDateTime::currentMSecsSinceEpoch() - lastUpdateCheck_ > 12LL * 60 * 60 * 1000)
                checkForUpdates(true);
        });
        timer_.start(kTimerMs);

        if (savedPower_)
            setPower(true);

        if (checkUpdates_)
            checkForUpdates(true);
    }

    ~MainWindow() override
    {
        saveSettings();
        stopEngine();
        CoUninitialize();
    }

protected:
    void closeEvent(QCloseEvent* event) override
    {
        if (tray_.isVisible() && !quitting_) {
            hide();
            event->ignore();
            tray_.showMessage("Audio Enhancer",
                              "The app is still running in the system tray.",
                              QSystemTrayIcon::Information,
                              2500);
        } else {
            event->accept();
        }
    }

private:
    AudioEngine engine_;
    std::vector<AudioDevice> devices_;
    std::wstring cableId_;
    std::wstring outputId_;
    std::wstring previousDefault_;

    int effect_ = 0;
    int volume_ = 100;
    int bass_ = 0;
    int treble_ = 0;
    bool savedPower_ = true;
    bool running_ = false;
    bool quitting_ = false;
    bool checkUpdates_ = true;
    QString theme_ = "default";
    QString mode_ = "system";
    QString updateUrl_;
    QString updateTag_;
    QString updateSha256_;
    QString updateFile_;
    quint64 lastUpdateCheck_ = 0;

    QTimer timer_;
    QSystemTrayIcon tray_;
    QAction* trayPower_ = nullptr;

    QComboBox* outputBox_ = nullptr;
    QComboBox* effectBox_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
    QSlider* bassSlider_ = nullptr;
    QSlider* trebleSlider_ = nullptr;
    QPushButton* powerButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* updateStatusLabel_ = nullptr;
    QPushButton* updateInstallButton_ = nullptr;
    QCheckBox* checkUpdatesToggle_ = nullptr;
    QNetworkAccessManager network_;

    static bool isCable(const AudioDevice& device)
    {
        return device.name.find(L"CABLE") != std::wstring::npos ||
               device.name.find(L"VB-Audio") != std::wstring::npos;
    }

    static bool is16Channel(const AudioDevice& device)
    {
        return device.name.find(L"16ch") != std::wstring::npos ||
               device.name.find(L"16 ch") != std::wstring::npos;
    }

    static int clampInt(int value, int low, int high)
    {
        return value < low ? low : (value > high ? high : value);
    }

    QSettings settings() const
    {
        return QSettings(QSettings::IniFormat,
                         QSettings::UserScope,
                         "AudioEnhancer",
                         "settings");
    }

    void loadSettings()
    {
        QSettings ini = settings();
        ini.beginGroup("Audio");
        effect_ = clampInt(ini.value("Effect", 0).toInt(), 0, 2);
        volume_ = clampInt(ini.value("Volume", 100).toInt(), 0, 100);
        bass_ = clampInt(ini.value("Bass", 100).toInt() - 100, -10, 10);
        treble_ = clampInt(ini.value("Treble", 100).toInt() - 100, -10, 10);
        savedPower_ = ini.value("Power", true).toBool();
        outputId_ = ini.value("Output", "").toString().toStdWString();
        ini.endGroup();

        ini.beginGroup("App");
        checkUpdates_ = ini.value("CheckUpdates", true).toBool();
        theme_ = ini.value("Theme", "default").toString();
        mode_ = ini.value("Mode", "system").toString();
        ini.endGroup();
    }

    void saveSettings()
    {
        QSettings ini = settings();
        ini.beginGroup("Audio");
        ini.setValue("Effect", effect_);
        ini.setValue("Volume", volume_);
        ini.setValue("Bass", bass_ + 100);
        ini.setValue("Treble", treble_ + 100);
        ini.setValue("Power", savedPower_);
        ini.setValue("Output", fromWide(outputId_));
        ini.endGroup();
        ini.beginGroup("App");
        ini.setValue("CheckUpdates", checkUpdates_);
        ini.setValue("Theme", theme_);
        ini.setValue("Mode", mode_);
        ini.endGroup();
        ini.sync();
    }

    void applyTone()
    {
        engine_.setEffect(effect_);
        engine_.setVolume(volume_);
        engine_.setBass(bass_);
        engine_.setTreble(treble_);
    }

    const AudioDevice* findOutput(const std::wstring& id) const
    {
        for (const auto& device : devices_)
            if (device.id == id && !isCable(device))
                return &device;
        return nullptr;
    }

    void refreshDevices()
    {
        const std::vector<AudioDevice> current = enumerateOutputDevices();

        bool changed = current.size() != devices_.size();
        if (!changed) {
            for (size_t i = 0; i < current.size(); ++i) {
                if (current[i].id != devices_[i].id || current[i].name != devices_[i].name) {
                    changed = true;
                    break;
                }
            }
        }
        devices_ = current;

        cableId_.clear();
        for (const auto& device : devices_)
            if (isCable(device) && !is16Channel(device)) {
                cableId_ = device.id;
                break;
            }
        if (cableId_.empty()) {
            for (const auto& device : devices_)
                if (isCable(device)) {
                    cableId_ = device.id;
                    break;
                }
        }

        if (!findOutput(outputId_)) {
            outputId_.clear();
            for (const auto& device : devices_)
                if (!isCable(device) && device.isDefault) {
                    outputId_ = device.id;
                    break;
                }
            if (outputId_.empty()) {
                for (const auto& device : devices_)
                    if (!isCable(device)) {
                        outputId_ = device.id;
                        break;
                    }
            }
            if (outputBox_)
                saveSettings();
        }

        if (changed || !outputBox_)
            rebuildOutputBox();

        if (running_ && !engine_.alive()) {
            stopEngine();
            setPower(false);
            statusLabel_->setText("Output disconnected. Reconnect the device and turn the enhancer on.");
        }
    }

    void rebuildOutputBox()
    {
        if (!outputBox_)
            return;

        const std::wstring selected = outputId_;
        outputBox_->blockSignals(true);
        outputBox_->clear();
        int selectedIndex = -1;
        for (const auto& device : devices_) {
            if (isCable(device))
                continue;
            const int index = outputBox_->count();
            outputBox_->addItem(fromWide(device.name), fromWide(device.id));
            if (device.id == selected)
                selectedIndex = index;
        }
        if (selectedIndex >= 0)
            outputBox_->setCurrentIndex(selectedIndex);
        outputBox_->blockSignals(false);
    }

    void restoreDefault()
    {
        std::wstring target;
        if (findOutput(previousDefault_))
            target = previousDefault_;
        else if (findOutput(outputId_))
            target = outputId_;
        else
            for (const auto& device : devices_)
                if (!isCable(device)) {
                    target = device.id;
                    break;
                }
        if (!target.empty())
            setDefaultOutput(target);
    }

    bool startEngine()
    {
        if (running_)
            return true;
        if (cableId_.empty()) {
            QMessageBox::warning(this,
                                 "Virtual audio device not found",
                                 "Install VB-Cable, restart Windows, then turn the enhancer on again.");
            statusLabel_->setText("VB-Cable is not installed.");
            return false;
        }
        if (!findOutput(outputId_)) {
            statusLabel_->setText("No output device is available.");
            return false;
        }

        previousDefault_ = getDefaultOutputId();
        if (previousDefault_ == cableId_)
            previousDefault_.clear();
        setDefaultOutput(cableId_);

        applyTone();
        std::wstring error;
        if (!engine_.start(cableId_, outputId_, error)) {
            restoreDefault();
            QMessageBox::warning(this, "Could not start", fromWide(error));
            statusLabel_->setText("Could not start the audio engine.");
            return false;
        }

        running_ = true;
        updateStatus();
        return true;
    }

    void stopEngine()
    {
        if (!running_)
            return;
        engine_.stop();
        running_ = false;
        restoreDefault();
        updateStatus();
    }

    void setPower(bool enabled)
    {
        if (enabled) {
            if (!startEngine()) {
                savedPower_ = false;
                powerButton_->setChecked(false);
                trayPower_->setChecked(false);
                saveSettings();
                return;
            }
        } else {
            stopEngine();
        }

        savedPower_ = enabled;
        powerButton_->setChecked(enabled);
        trayPower_->setChecked(enabled);
        updateStatus();
        saveSettings();
    }

    void updateStatus()
    {
        if (running_) {
            const AudioDevice* output = findOutput(outputId_);
            const QString name = output ? fromWide(output->name) : "Selected output";
            statusLabel_->setText("Active  -  " + name);
            powerButton_->setText("Turn Off");
            tray_.setIcon(QIcon(":/app.ico"));
            tray_.setToolTip("Audio Enhancer - Active");
        } else {
            statusLabel_->setText("Inactive");
            powerButton_->setText("Turn On");
            tray_.setIcon(QIcon(":/app-off.ico"));
            tray_.setToolTip("Audio Enhancer - Off");
        }
    }

    void buildUi()
    {
        auto* rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(16, 16, 16, 16);

        auto* title = new QLabel("Audio Enhancer");
        QFont titleFont = title->font();
        titleFont.setPointSize(18);
        titleFont.setBold(true);
        title->setFont(titleFont);
        rootLayout->addWidget(title);

        auto* tabs = new QTabWidget();
        rootLayout->addWidget(tabs, 1);

        auto* mainPage = new QWidget();
        auto* mainLayout = new QVBoxLayout(mainPage);
        mainLayout->setContentsMargins(8, 8, 8, 8);
        mainLayout->setSpacing(12);

        auto* audioGroup = new QGroupBox("Audio");
        auto* audioLayout = new QVBoxLayout(audioGroup);

        auto* outputRow = new QHBoxLayout();
        outputRow->addWidget(new QLabel("Audio Output"));
        outputBox_ = new QComboBox();
        outputRow->addWidget(outputBox_, 1);
        audioLayout->addLayout(outputRow);

        auto* effectRow = new QHBoxLayout();
        effectRow->addWidget(new QLabel("Audio Effect"));
        effectBox_ = new QComboBox();
        effectBox_->addItems({"Normal", "3D", "Balanced"});
        effectBox_->setCurrentIndex(effect_);
        effectRow->addWidget(effectBox_, 1);
        audioLayout->addLayout(effectRow);

        volumeSlider_ = addSlider(audioLayout, "Volume", 0, 100, volume_);
        bassSlider_ = addSlider(audioLayout, "Bass", -10, 10, bass_);
        trebleSlider_ = addSlider(audioLayout, "Treble", -10, 10, treble_);
        mainLayout->addWidget(audioGroup);

        statusLabel_ = new QLabel("Inactive");
        statusLabel_->setAlignment(Qt::AlignCenter);
        mainLayout->addWidget(statusLabel_);

        powerButton_ = new QPushButton(savedPower_ ? "Turn Off" : "Turn On");
        powerButton_->setCheckable(true);
        powerButton_->setChecked(savedPower_);
        powerButton_->setMinimumHeight(42);
        mainLayout->addWidget(powerButton_);
        mainLayout->addStretch();
        tabs->addTab(mainPage, "Main");

        auto* settingsPage = new QWidget();
        auto* settingsLayout = new QVBoxLayout(settingsPage);
        settingsLayout->setContentsMargins(8, 8, 8, 8);

        auto* appearance = new QGroupBox("Appearance");
        auto* appearanceLayout = new QVBoxLayout(appearance);

        appearanceLayout->addWidget(new QLabel("Theme"));
        auto* themeRow = new QHBoxLayout();
        const QStringList themes = {"default", "dracula", "white", "nord", "midnight",
                                    "red-velvet", "amber", "ocean", "forest"};
        for (const QString& id : themes) {
            auto* button = new QPushButton(id);
            button->setCheckable(true);
            button->setChecked(id == theme_);
            themeRow->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, id]() {
                theme_ = id;
                applyAppearance();
                saveSettings();
            });
        }
        appearanceLayout->addLayout(themeRow);

        appearanceLayout->addWidget(new QLabel("Mode"));
        auto* modeRow = new QHBoxLayout();
        for (const QString& id : QStringList{"system", "light", "dark"}) {
            auto* button = new QPushButton(id);
            button->setCheckable(true);
            button->setChecked(id == mode_);
            modeRow->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, id]() {
                mode_ = id;
                applyAppearance();
                saveSettings();
            });
        }
        appearanceLayout->addLayout(modeRow);
        settingsLayout->addWidget(appearance);

        auto* appSettings = new QGroupBox("Application");
        auto* appLayout = new QVBoxLayout(appSettings);
        auto* autostartLabel = new QLabel("Startup: Enabled for all Windows users");
        autostartLabel->setEnabled(false);
        appLayout->addWidget(autostartLabel);

        checkUpdatesToggle_ = new QCheckBox("Automatically check for updates");
        checkUpdatesToggle_->setChecked(checkUpdates_);
        connect(checkUpdatesToggle_, &QCheckBox::toggled, this, [this](bool checked) {
            checkUpdates_ = checked;
            saveSettings();
        });
        appLayout->addWidget(checkUpdatesToggle_);
        auto* linksRow = new QHBoxLayout();
        auto* githubButton = new QPushButton("GitHub");
        auto* vbButton = new QPushButton("VB-Cable");
        auto* websiteButton = new QPushButton("Website");
        linksRow->addWidget(githubButton);
        linksRow->addWidget(vbButton);
        linksRow->addWidget(websiteButton);
        appLayout->addLayout(linksRow);
        settingsLayout->addWidget(appSettings);

        auto* updates = new QGroupBox("Updates");
        auto* updatesLayout = new QVBoxLayout(updates);
        updateStatusLabel_ = new QLabel("Current version: " APP_VERSION_STR);
        updatesLayout->addWidget(updateStatusLabel_);

        auto* updateButtons = new QHBoxLayout();
        auto* checkButton = new QPushButton("Check for Updates");
        updateInstallButton_ = new QPushButton("Install Update");
        updateInstallButton_->setEnabled(false);
        updateButtons->addWidget(checkButton);
        updateButtons->addWidget(updateInstallButton_);
        updatesLayout->addLayout(updateButtons);
        settingsLayout->addWidget(updates);
        settingsLayout->addStretch();
        tabs->addTab(settingsPage, "Settings");

        rebuildOutputBox();
        applyAppearance();

        connect(outputBox_, &QComboBox::currentIndexChanged, this, [this](int index) {
            if (index < 0)
                return;
            outputId_ = outputBox_->itemData(index).toString().toStdWString();
            saveSettings();
            if (running_) {
                engine_.stop();
                running_ = false;
                restoreDefault();
                setPower(true);
            }
        });

        connect(effectBox_, &QComboBox::currentIndexChanged, this, [this](int index) {
            effect_ = clampInt(index, 0, 2);
            engine_.setEffect(effect_);
            saveSettings();
            updateStatus();
        });

        connect(volumeSlider_, &QSlider::valueChanged, this, [this](int value) {
            volume_ = value;
            engine_.setVolume(value);
            saveSettings();
        });
        connect(bassSlider_, &QSlider::valueChanged, this, [this](int value) {
            bass_ = clampInt(value, -10, 10);
            engine_.setBass(bass_);
            saveSettings();
        });
        connect(trebleSlider_, &QSlider::valueChanged, this, [this](int value) {
            treble_ = clampInt(value, -10, 10);
            engine_.setTreble(treble_);
            saveSettings();
        });
        connect(powerButton_, &QPushButton::clicked, this, [this](bool checked) {
            setPower(checked);
        });
        connect(checkButton, &QPushButton::clicked, this, [this]() {
            checkForUpdates(false);
        });
        connect(updateInstallButton_, &QPushButton::clicked, this, [this]() {
            startUpdateDownload();
        });
        connect(githubButton, &QPushButton::clicked, this, []() {
            QDesktopServices::openUrl(QUrl("https://github.com/mahfuz0712/audioEnhancer"));
        });
        connect(vbButton, &QPushButton::clicked, this, []() {
            QDesktopServices::openUrl(QUrl("https://vb-audio.com/Cable"));
        });
        connect(websiteButton, &QPushButton::clicked, this, []() {
            QDesktopServices::openUrl(QUrl("https://portfolio-mahfuz0712.vercel.app"));
        });
    }

    static int compareVersions(const QString& left, const QString& right)
    {
        auto parts = [](const QString& value) {
            QString s = value;
            if (!s.isEmpty() && (s.front() == 'v' || s.front() == 'V'))
                s.remove(0, 1);
            return s.split('.', Qt::SkipEmptyParts);
        };
        const QStringList a = parts(left);
        const QStringList b = parts(right);
        const int count = qMax(a.size(), b.size());
        for (int i = 0; i < count; ++i) {
            const int av = i < a.size() ? a.at(i).toInt() : 0;
            const int bv = i < b.size() ? b.at(i).toInt() : 0;
            if (av != bv)
                return av < bv ? -1 : 1;
        }
        return 0;
    }

    void checkForUpdates(bool silent)
    {
        if (!updateStatusLabel_)
            return;
        lastUpdateCheck_ = QDateTime::currentMSecsSinceEpoch();
        if (!silent)
            updateStatusLabel_->setText("Checking for updates...");

        QNetworkRequest request(QUrl("https://api.github.com/repos/mahfuz0712/audioEnhancer/releases/latest"));
        request.setHeader(QNetworkRequest::UserAgentHeader, "AudioEnhancer/" APP_VERSION_STR);
        request.setRawHeader("Accept", "application/vnd.github+json");

        QNetworkReply* reply = network_.get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, silent]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                if (!silent)
                    updateStatusLabel_->setText("Update check failed: " + reply->errorString());
                return;
            }

            const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
            const QJsonObject release = document.object();
            updateTag_ = release.value("tag_name").toString();
            if (compareVersions(updateTag_, APP_VERSION_STR) <= 0) {
                updateUrl_.clear();
                updateInstallButton_->setEnabled(false);
                updateStatusLabel_->setText("You are up to date (" APP_VERSION_STR ").");
                return;
            }

            const QJsonArray assets = release.value("assets").toArray();
            for (const QJsonValue& value : assets) {
                const QJsonObject asset = value.toObject();
                const QString name = asset.value("name").toString();
                if (!name.endsWith(".exe", Qt::CaseInsensitive))
                    continue;
                updateUrl_ = asset.value("browser_download_url").toString();
                updateSha256_.clear();
                const QString digest = asset.value("digest").toString();
                if (digest.startsWith("sha256:", Qt::CaseInsensitive))
                    updateSha256_ = digest.mid(7);
                break;
            }

            if (updateUrl_.isEmpty()) {
                updateInstallButton_->setEnabled(false);
                updateStatusLabel_->setText("Version " + updateTag_ + " is available, but no Windows installer was found.");
            } else {
                updateInstallButton_->setEnabled(true);
                updateStatusLabel_->setText("Update available: " + updateTag_);
                tray_.showMessage("Audio Enhancer update available",
                                  "Version " + updateTag_ + " is available.",
                                  QSystemTrayIcon::Information,
                                  4000);
            }
        });
    }

    void startUpdateDownload()
    {
        if (updateUrl_.isEmpty())
            return;
        updateInstallButton_->setEnabled(false);
        updateStatusLabel_->setText("Downloading " + updateTag_ + "...");

        QNetworkRequest request{QUrl(updateUrl_)};
        request.setHeader(QNetworkRequest::UserAgentHeader, "AudioEnhancer/" APP_VERSION_STR);
        QNetworkReply* reply = network_.get(request);
        connect(reply, &QNetworkReply::downloadProgress,
                this, [this](qint64 received, qint64 total) {
                    if (total > 0)
                        updateStatusLabel_->setText(QString("Downloading update: %1%")
                            .arg(received * 100 / total));
                });

        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                updateStatusLabel_->setText("Download failed: " + reply->errorString());
                updateInstallButton_->setEnabled(true);
                return;
            }

            const QString tempDir = QDir::temp().filePath("AudioEnhancer");
            QDir().mkpath(tempDir);
            updateFile_ = QDir(tempDir).filePath("AudioEnhancer-" + updateTag_ + "-Setup.exe");
            QFile file(updateFile_);
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(reply->readAll()) < 0) {
                updateStatusLabel_->setText("Could not write the downloaded installer.");
                updateInstallButton_->setEnabled(true);
                return;
            }
            file.close();

            if (!updateSha256_.isEmpty()) {
                if (!file.open(QIODevice::ReadOnly)) {
                    updateStatusLabel_->setText("Could not verify the downloaded installer.");
                    updateInstallButton_->setEnabled(true);
                    return;
                }
                QCryptographicHash hash(QCryptographicHash::Sha256);
                if (!hash.addData(&file) || hash.result().toHex().toLower() != updateSha256_.toLower()) {
                    file.close();
                    QFile::remove(updateFile_);
                    updateStatusLabel_->setText("Checksum mismatch. The installer was not started.");
                    updateInstallButton_->setEnabled(true);
                    return;
                }
                file.close();
            }

            updateStatusLabel_->setText("Installer downloaded. Starting setup...");
            const QString executable = updateFile_;
            SHELLEXECUTEINFOW info = {};
            info.cbSize = sizeof(info);
            info.fMask = SEE_MASK_NOCLOSEPROCESS;
            info.lpVerb = L"runas";
            info.lpFile = reinterpret_cast<LPCWSTR>(executable.utf16());
            info.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&info)) {
                quitting_ = true;
                tray_.hide();
                qApp->quit();
            } else {
                updateStatusLabel_->setText("Could not start the installer.");
                updateInstallButton_->setEnabled(true);
            }
        });
    }

    void applyAppearance()
    {
        bool dark = mode_ == "dark";
        if (mode_ == "system") {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
            dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
            dark = palette().color(QPalette::Window).lightness() < 128;
#endif
        }

        QPalette palette = QApplication::palette();
        if (dark) {
            palette.setColor(QPalette::Window, QColor("#171717"));
            palette.setColor(QPalette::Base, QColor("#252525"));
            palette.setColor(QPalette::Text, QColor("#f3f3f3"));
            palette.setColor(QPalette::WindowText, QColor("#f3f3f3"));
            palette.setColor(QPalette::Button, QColor("#303030"));
            palette.setColor(QPalette::ButtonText, QColor("#ffffff"));
        } else {
            palette = QApplication::style()->standardPalette();
        }
        QApplication::setPalette(palette);
    }

    QSlider* addSlider(QVBoxLayout* layout, const QString& label, int low, int high, int value)
    {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(label));
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setRange(low, high);
        slider->setValue(value);
        row->addWidget(slider, 1);
        auto* valueLabel = new QLabel(QString::number(value));
        valueLabel->setMinimumWidth(40);
        valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(valueLabel);
        connect(slider, &QSlider::valueChanged, valueLabel, [valueLabel](int current) {
            valueLabel->setText(QString::number(current));
        });
        layout->addLayout(row);
        return slider;
    }

    void buildTray()
    {
        tray_.setIcon(QIcon(":/app-off.ico"));
        tray_.setToolTip("Audio Enhancer");

        auto* menu = new QMenu(this);
        auto* openAction = menu->addAction("Open");
        connect(openAction, &QAction::triggered, this, [this]() {
            showNormal();
            activateWindow();
        });

        trayPower_ = menu->addAction("Enabled");
        trayPower_->setCheckable(true);
        trayPower_->setChecked(savedPower_);
        connect(trayPower_, &QAction::triggered, this, [this](bool checked) {
            setPower(checked);
        });


        menu->addSeparator();
        auto* exitAction = menu->addAction("Exit");
        connect(exitAction, &QAction::triggered, this, [this]() {
            quitting_ = true;
            tray_.hide();
            qApp->quit();
        });

        tray_.setContextMenu(menu);
        connect(&tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger) {
                showNormal();
                activateWindow();
            }
        });
        tray_.show();
        updateStatus();
    }
};

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("Audio Enhancer");
    app.setOrganizationName("Mahfuz Rahman");

    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\AudioEnhancer_SingleInstance");
    if (singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"QWidget", nullptr);
        if (existing)
            PostMessageW(existing, WM_APP + 1, 0, 0);
        CloseHandle(singleInstance);
        return 0;
    }

    MainWindow window;
    if (app.arguments().contains("--tray"))
        window.hide();
    else
        window.show();

    const int result = app.exec();
    if (singleInstance)
        CloseHandle(singleInstance);
    return result;
}
