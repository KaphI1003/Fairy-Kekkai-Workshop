#include "view/superres_interface.h"

#include <QDesktopServices>
#include <QDialog>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QMimeData>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QUrl>
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <cmath>
#include "common/app_data.h"
#include "common/config.h"
#include "common/event_bus.h"
#include "common/setting.h"
#include "components/notification_service.h"
#include "service/video_frame_service.h"
#include "service/video_preview.h"

namespace fkw {
namespace {
class InlineSuperresCard : public qfw::SettingCard {
public:
    using qfw::SettingCard::SettingCard;
    QHBoxLayout* row() { return hBoxLayout_; }
};
void save(const char* key, const QJsonValue& value) {
    AppConfig::instance().set(QStringLiteral("Superres"), QString::fromLatin1(key), value);
}
QString timeText(double seconds) {
    const int value = qMax(0, int(seconds));
    return QStringLiteral("%1:%2:%3").arg(value / 3600, 2, 10, QLatin1Char('0'))
        .arg(value / 60 % 60, 2, 10, QLatin1Char('0')).arg(value % 60, 2, 10, QLatin1Char('0'));
}
QString previewTimeText(double seconds) {
    const qint64 milliseconds = qMax<qint64>(0, qRound64(seconds * 1000));
    const QString whole = timeText(milliseconds / 1000);
    return (milliseconds < 3600000 ? whole.mid(3) : whole) +
        QStringLiteral(".%1").arg(milliseconds % 1000, 3, 10, QLatin1Char('0'));
}
qfw::ComboBox* choice(qfw::SettingCardGroup* group, const QString& title,
                     const QString& description, const QStringList& labels, const QStringList& values,
                     const QString& selected, const char* key, qfw::SettingCard** cardOut = nullptr) {
    auto* card = new DictSettingCard(qfw::FluentIconEnum::Video, title, description, labels, group);
    card->comboBox->setMinimumWidth(175);
    card->comboBox->setCurrentIndex(qMax(0, values.indexOf(selected)));
    QObject::connect(card->comboBox, &qfw::ComboBox::currentIndexChanged, card, [values, key](int index) {
        if (index >= 0) {
            if (QString::fromLatin1(key) == QStringLiteral("PresetHeight") || QString::fromLatin1(key) == QStringLiteral("Scale")) save(key, values.at(index).toInt());
            else save(key, values.at(index));
        }
    });
    group->addSettingCard(card);
    if (cardOut) *cardOut = card;
    return card->comboBox;
}
qfw::SpinBox* integerCard(qfw::SettingCardGroup* group, const QString& title, const QString& description,
                         int value, int minimum, int maximum, const char* key) {
    auto* card = new InlineSuperresCard(qfw::FluentIconEnum::Setting, title, description, group);
    auto* spin = new qfw::SpinBox(card);
    spin->setRange(minimum, maximum); spin->setValue(value); spin->setMinimumWidth(150);
    card->row()->addWidget(spin); card->row()->addSpacing(16);
    QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), card, [key](int n) { save(key, n); });
    group->addSettingCard(card);
    return spin;
}

// Both views read the same frame index. Playback and stepping therefore compare
// corresponding source/render frames instead of launching unsynchronised players.
class SuperresPreviewDialog : public QDialog {
public:
    SuperresPreviewDialog(const QString& source, const QString& result, double start, QWidget* parent)
        : QDialog(parent), source_(source.toStdString()), result_(result.toStdString()) {
        setWindowTitle(trText("视频超分预览")); resize(1000, 650);
        auto* layout = new QVBoxLayout(this);
        auto* headings = new QHBoxLayout;
        headings->addWidget(new qfw::BodyLabel(trText("原视频"), this), 1);
        headings->addWidget(new qfw::BodyLabel(trText("超分结果"), this), 1);
        layout->addLayout(headings);
        auto* row = new QHBoxLayout;
        before_ = new QLabel(this); after_ = new QLabel(this);
        for (auto* label : {before_, after_}) { label->setAlignment(Qt::AlignCenter); label->setMinimumSize(160, 120); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored); row->addWidget(label, 1); }
        layout->addLayout(row, 1);
        slider_ = new qfw::Slider(Qt::Horizontal, this);
        const int frames = int(result_.get(cv::CAP_PROP_FRAME_COUNT));
        fps_ = result_.get(cv::CAP_PROP_FPS);
        if (fps_ <= 0) fps_ = 30;
        offset_ = qRound64(start * source_.get(cv::CAP_PROP_FPS));
        slider_->setRange(0, qMax(0, frames - 1));
        layout->addWidget(slider_);
        auto* controls = new QHBoxLayout;
        auto* previous = new qfw::PushButton(trText("上一帧"), this);
        play_ = new qfw::PushButton(trText("播放"), this);
        auto* next = new qfw::PushButton(trText("下一帧"), this);
        auto* open = new qfw::PushButton(trText("用播放器打开"), this);
        frameLabel_ = new qfw::BodyLabel(this);
        for (auto* button : {previous, play_, next, open}) controls->addWidget(button);
        controls->addWidget(frameLabel_); controls->addStretch(); layout->addLayout(controls);
        timer_.setInterval(qMax(1, qRound(1000 / fps_)));
        connect(&timer_, &QTimer::timeout, this, [this]() {
            if (slider_->value() >= slider_->maximum()) { timer_.stop(); play_->setText(trText("播放")); }
            else slider_->setValue(slider_->value() + 1);
        });
        connect(play_, &QPushButton::clicked, this, [this]() {
            if (timer_.isActive()) { timer_.stop(); play_->setText(trText("播放")); }
            else { if (slider_->value() == slider_->maximum()) slider_->setValue(0); timer_.start(); play_->setText(trText("暂停")); }
        });
        connect(previous, &QPushButton::clicked, this, [this]() { slider_->setValue(slider_->value() - 1); });
        connect(next, &QPushButton::clicked, this, [this]() { slider_->setValue(slider_->value() + 1); });
        connect(open, &QPushButton::clicked, this, [result]() { QDesktopServices::openUrl(QUrl::fromLocalFile(result)); });
        connect(slider_, &QSlider::valueChanged, this, [this](int) { display(); });
        display();
    }
protected:
    void resizeEvent(QResizeEvent* event) override { QDialog::resizeEvent(event); display(); }
private:
    void display() {
        if (!slider_) return;
        const int frame = slider_->value();
        auto show = [](cv::VideoCapture& capture, qint64 index, QLabel* label) {
            if (qRound64(capture.get(cv::CAP_PROP_POS_FRAMES)) != index) capture.set(cv::CAP_PROP_POS_FRAMES, double(index));
            cv::Mat image;
            if (!capture.read(image) || image.empty() || image.type() != CV_8UC3) { label->setText(trText("无法读取预览帧")); return; }
            const QImage picture(image.data, image.cols, image.rows, int(image.step), QImage::Format_BGR888);
            label->setPixmap(QPixmap::fromImage(picture).scaled(label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        };
        show(source_, offset_ + frame, before_); show(result_, frame, after_);
        frameLabel_->setText(QStringLiteral("%1/%2 · %3s").arg(frame + 1).arg(slider_->maximum() + 1).arg(frame / fps_, 0, 'f', 2));
    }
    cv::VideoCapture source_, result_;
    QLabel* before_ = nullptr;
    QLabel* after_ = nullptr;
    qfw::Slider* slider_ = nullptr;
    qfw::BodyLabel* frameLabel_ = nullptr;
    qfw::PushButton* play_ = nullptr;
    QTimer timer_;
    qint64 offset_ = 0;
    double fps_ = 30;
};
}

SuperresInterface::SuperresInterface(QWidget* parent)
    : BaseFunctionInterface(trText("超分"), qfw::FluentIconEnum::Video, parent), probe_(new QProcess(this)) {
    setObjectName(QStringLiteral("superresInterface"));
    setOutputSuffix(QStringLiteral("_SR.mp4"));
    setFileFilter(QStringLiteral("*.mp4;*.mkv;*.mov;*.avi;*.webm;*.flv;*.m4v;*.ts"));
    const auto initial = SuperresOptions::fromConfig();
    auto* sourceCard = new InlineSuperresCard(qfw::FluentIconEnum::Info, trText("视频信息"), QString(), fileSelectionGroup_);
    sourceInfo_ = new qfw::BodyLabel(trText("选择视频后读取尺寸、帧率、时长和音轨"), sourceCard);
    sourceInfo_->setObjectName(QStringLiteral("superresSourceInfo"));
    sourceInfo_->setWordWrap(true); sourceCard->row()->addWidget(sourceInfo_, 1); sourceCard->row()->addSpacing(16);
    fileSelectionGroup_->addSettingCard(sourceCard);
    sizeMode_ = choice(settingsGroup_, trText("尺寸设置"), trText("选择目标分辨率、放大倍数或自定义尺寸"),
        {trText("常用分辨率"), trText("按倍数放大"), trText("自定义宽高")}, {"preset", "scale", "custom"}, initial.sizeMode, "SizeMode");
    sizeMode_->setObjectName(QStringLiteral("superresSizeMode"));
    preset_ = choice(settingsGroup_, trText("目标分辨率"), trText("保持原比例，自动适配横屏和竖屏"),
        {"720p", "1080p", "1440p", "2160p / 4K"}, {"720", "1080", "1440", "2160"}, QString::number(initial.presetHeight), "PresetHeight", &presetCard_);
    scale_ = choice(settingsGroup_, trText("放大倍数"), trText("以原始视频尺寸为基准"), {trText("2 倍"), trText("3 倍"), trText("4 倍")},
        {"2", "3", "4"}, QString::number(initial.scale), "Scale", &scaleCard_);
    auto* custom = new InlineSuperresCard(qfw::FluentIconEnum::Setting, trText("自定义宽高"), trText("输出尺寸自动对齐为偶数像素"), settingsGroup_);
    customRow_ = custom;
    width_ = new qfw::SpinBox(custom); height_ = new qfw::SpinBox(custom);
    width_->setObjectName(QStringLiteral("superresWidth")); height_->setObjectName(QStringLiteral("superresHeight"));
    for (auto* spin : {width_, height_}) { spin->setRange(2, 16384); spin->setSingleStep(2); spin->setFixedWidth(110); }
    width_->setValue(initial.width); height_->setValue(initial.height);
    lockRatio_ = new qfw::CheckBox(trText("锁定比例"), custom); lockRatio_->setChecked(initial.lockRatio);
    custom->row()->addWidget(width_); custom->row()->addWidget(new qfw::BodyLabel(QStringLiteral("×"), custom));
    custom->row()->addWidget(height_); custom->row()->addWidget(lockRatio_); custom->row()->addSpacing(16);
    settingsGroup_->addSettingCard(custom);
    fit_ = choice(settingsGroup_, trText("比例不一致时"), trText("保持画面比例，避免拉伸"), {trText("补边保留完整画面"), trText("居中裁剪填满画面")}, {"pad", "crop"}, initial.fit, "Fit");
    model_ = choice(settingsGroup_, trText("超分模型"), trText("用于东方 MMD；建议先预览头发、衣服与舞台光影"),
        {trText("动漫视频（速度优先）"), trText("通用细节（对比）"), trText("动漫插图（对比）"), QStringLiteral("2× AniSD SPAN")},
        superresModelIds(), initial.model, "Model");
    model_->setObjectName(QStringLiteral("superresModel"));
    reuseFrames_ = new qfw::SwitchSettingCard(qfw::FluentIconEnum::Sync, trText("相同帧复用"),
        trText("完全相同的画面只超分一次，保留原始帧率与时长"), nullptr, settingsGroup_);
    reuseFrames_->setObjectName(QStringLiteral("superresReuseFrames"));
    reuseFrames_->setChecked(initial.reuseIdenticalFrames);
    connect(reuseFrames_, &qfw::SwitchSettingCard::checkedChanged, this, [](bool checked) { save("ReuseIdenticalFrames", checked); });
    settingsGroup_->addSettingCard(reuseFrames_);
    quality_ = choice(settingsGroup_, trText("编码画质"), trText("影响结果的压缩质量与文件大小"), {trText("均衡"), trText("高画质"), trText("小体积")},
        {"balanced", "high", "small"}, initial.quality, "Quality");
    codec_ = choice(settingsGroup_, trText("视频编码"), QString(), {"H.264", "H.265"}, {"libx264", "libx265"}, initial.codec, "Codec");
    container_ = choice(settingsGroup_, trText("输出格式"), QString(), {"MP4", "MKV"}, {"mp4", "mkv"}, initial.container, "Container");
    audio_ = choice(settingsGroup_, trText("音频处理"), trText("保持原始帧率；音频格式不兼容时可选 AAC"),
        {trText("保留原音频"), trText("转为 AAC"), trText("移除音频")}, {"copy", "aac", "none"}, initial.audio, "Audio");
    targetInfo_ = new qfw::BodyLabel(this); targetInfo_->setWordWrap(true);
    mainLayout_->insertWidget(mainLayout_->count() - 2, targetInfo_);

    auto* preview = new qfw::SimpleCardWidget(this);
    auto* previewLayout = new QVBoxLayout(preview);
    previewLayout->setContentsMargins(20, 20, 20, 20);
    previewLayout->addWidget(new qfw::StrongBodyLabel(trText("视频预览"), preview));
    sourceCanvas_ = new PreviewCanvas(preview);
    sourceCanvas_->setObjectName(QStringLiteral("superresSourcePreview"));
    previewLayout->addWidget(sourceCanvas_);
    auto* position = new QHBoxLayout;
    sourceSlider_ = new qfw::Slider(Qt::Horizontal, preview);
    sourceSlider_->setObjectName(QStringLiteral("superresSourceSlider"));
    sourceSlider_->setTracking(true); sourceSlider_->setEnabled(false);
    sourceFrameLabel_ = new qfw::CaptionLabel(trText("帧：-/-"), preview);
    sourceTimeLabel_ = new qfw::CaptionLabel(trText("起点：--:--.---"), preview);
    sourceTimeLabel_->setObjectName(QStringLiteral("superresSourceTime"));
    position->addWidget(sourceSlider_, 4);
    position->addWidget(sourceFrameLabel_);
    position->addWidget(sourceTimeLabel_);
    previewLayout->addLayout(position);
    auto* hint = new qfw::BodyLabel(trText("拖动进度条选择起点，从当前画面开始预览最多 5 秒。"), preview);
    hint->setWordWrap(true); previewLayout->addWidget(hint);
    auto* controls = new QHBoxLayout;
    previewButton_ = new qfw::PushButton(trText("预览 5 秒效果"), preview); previewButton_->setEnabled(false);
    previewButton_->setObjectName(QStringLiteral("superresPreviewButton"));
    cancelPreview_ = new qfw::PushButton(trText("取消预览"), preview); cancelPreview_->hide();
    viewPreview_ = new qfw::PushButton(trText("查看预览对比"), preview); viewPreview_->hide();
    controls->addWidget(previewButton_); controls->addWidget(cancelPreview_); controls->addWidget(viewPreview_); controls->addStretch();
    previewLayout->addLayout(controls);
    previewProgress_ = new qfw::ProgressBar(preview, false); previewProgress_->hide();
    previewProgress_->setObjectName(QStringLiteral("superresPreviewProgress"));
    previewStatus_ = new qfw::BodyLabel(preview); previewStatus_->setWordWrap(true);
    previewStatus_->setObjectName(QStringLiteral("superresPreviewStatus"));
    previewLayout->addWidget(previewProgress_); previewLayout->addWidget(previewStatus_);
    previewReuseInfo_ = new qfw::CaptionLabel(preview);
    previewReuseInfo_->setObjectName(QStringLiteral("superresPreviewReuseInfo")); previewReuseInfo_->setWordWrap(true);
    previewLayout->addWidget(previewReuseInfo_);
    insertPreview(preview);

    sourceFrames_ = new VideoFrameService(this);
    seekDelay_.setSingleShot(true); seekDelay_.setInterval(50);
    connect(sourceFrames_, &VideoFrameService::videoOpened, this, [this](int count, double, double) {
        const QSignalBlocker blocker(sourceSlider_);
        sourceSlider_->setRange(0, qMax(0, count - 1)); sourceSlider_->setValue(0);
        sourceSlider_->setEnabled(count > 1);
        updateSourcePosition(); updatePreviewAvailability();
    });
    connect(sourceSlider_, &QSlider::valueChanged, this, [this](int) {
        updateSourcePosition(); updatePreviewAvailability();
        // Match the subtitle page: keep rendering during a continuous drag,
        // while the shared frame service coalesces pending seeks to the latest.
        if (!seekDelay_.isActive()) seekDelay_.start();
    });
    connect(sourceSlider_, &QSlider::sliderReleased, this, [this]() {
        seekDelay_.stop(); sourceFrames_->requestFrame(sourceSlider_->sliderPosition());
    });
    connect(&seekDelay_, &QTimer::timeout, this, [this]() { sourceFrames_->requestFrame(sourceSlider_->value()); });
    connect(sourceFrames_, &VideoFrameService::frameReady, this, [this](int index, const QImage& image) {
        sourceCanvas_->setFrame(image); displayedSourceFrame_ = index;
        updateSourcePosition(); updatePreviewAvailability();
    });
    connect(sourceFrames_, &VideoFrameService::failed, this, [this](const QString& error) {
        displayedSourceFrame_ = -1;
        sourceCanvas_->setFrame({}); sourceCanvas_->setText(error);
        updatePreviewAvailability();
    });
    probeDelay_.setSingleShot(true); probeDelay_.setInterval(200);
    connect(&probeDelay_, &QTimer::timeout, this, &SuperresInterface::probeVideo);
    connect(inputFileCard_->lineEdit, &QLineEdit::textChanged, this, [this]() {
        probedPath_.clear(); videoInfo_ = {}; generatedOutput_.clear();
        seekDelay_.stop(); sourceFrames_->open({}); sourceVideoPath_.clear();
        displayedSourceFrame_ = -1; sourceCanvas_->setFrame({});
        {
            const QSignalBlocker blocker(sourceSlider_);
            sourceSlider_->setRange(0, 0); sourceSlider_->setValue(0); sourceSlider_->setEnabled(false);
        }
        updateSourcePosition();
        startButton_->setEnabled(false); previewButton_->setEnabled(false); probeDelay_.start();
    });
    connect(probe_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
        if (probe_->property("inputPath").toString() != inputPath()) return;
        videoInfo_ = code == 0 && status == QProcess::NormalExit ? parseSuperresVideoInfo(QJsonDocument::fromJson(probe_->readAllStandardOutput()).object()) : SuperresVideoInfo{};
        probedPath_ = inputPath();
        const bool valid = !videoInfo_.size.isEmpty() && videoInfo_.fps > 0;
        sourceInfo_->setText(valid ? QStringLiteral("%1 × %2 · %3 fps · %4 · %5").arg(videoInfo_.size.width()).arg(videoInfo_.size.height())
            .arg(videoInfo_.fps, 0, 'f', 3).arg(timeText(videoInfo_.duration)).arg(videoInfo_.hasAudio ? trText("有音频") : trText("无音频"))
            : trText("无法读取视频信息，请在高级设置中检查 ffprobe 路径。"));
        startButton_->setEnabled(valid); updatePreviewAvailability();
        updateSourcePosition(); refreshTarget();
    });
    connect(probe_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) sourceInfo_->setText(trText("无法启动 ffprobe，请在高级设置中检查路径。"));
    });
    for (auto* combo : {sizeMode_, preset_, scale_, model_, quality_, codec_, audio_, container_, fit_})
        connect(combo, &qfw::ComboBox::currentIndexChanged, this, [this](int) { refreshTarget(); });
    const auto resize = [this](bool widthChanged) {
        if (lockRatio_->isChecked() && !videoInfo_.size.isEmpty()) {
            auto* other = widthChanged ? height_ : width_;
            QSignalBlocker blocker(other);
            other->setValue(qMax(2, qRound((widthChanged ? width_->value() * double(videoInfo_.size.height()) / videoInfo_.size.width()
                : height_->value() * double(videoInfo_.size.width()) / videoInfo_.size.height()) / 2) * 2));
        }
        save("Width", width_->value()); save("Height", height_->value()); refreshTarget();
    };
    connect(width_, qOverload<int>(&QSpinBox::valueChanged), this, [resize](int) { resize(true); });
    connect(height_, qOverload<int>(&QSpinBox::valueChanged), this, [resize](int) { resize(false); });
    connect(lockRatio_, &QCheckBox::toggled, this, [resize](bool checked) { save("LockRatio", checked); resize(true); });
    connect(previewButton_, &QPushButton::clicked, this, &SuperresInterface::startPreview);
    connect(cancelPreview_, &QPushButton::clicked, this, [this]() {
        cancelPreview_->setEnabled(false); previewStatus_->setText(trText("正在取消预览…"));
        if (previewWorker_) emit cancelPreviewRequested(previewWorker_);
    });
    connect(viewPreview_, &QPushButton::clicked, this, &SuperresInterface::showPreview);
    connect(&AppConfig::instance(), &AppConfig::valueChanged, this, [this](const QString& group, const QString& key, const QJsonValue&) {
        if (group == QStringLiteral("Superres") && key == QStringLiteral("FFprobePath") && !inputPath().isEmpty()) probeDelay_.start();
    });
    refreshTarget();
}

SuperresInterface::~SuperresInterface() {
    if (previewWorker_) previewWorker_->cancel();
    if (probe_->state() != QProcess::NotRunning) { probe_->kill(); probe_->waitForFinished(1000); }
    if (!previewPath_.isEmpty()) QFile::remove(previewPath_);
}

SuperresOptions SuperresInterface::options() const {
    auto result = SuperresOptions::fromConfig();
    result.sizeMode = QStringList{"preset", "scale", "custom"}.value(sizeMode_->currentIndex());
    result.presetHeight = QList<int>{720, 1080, 1440, 2160}.value(preset_->currentIndex(), 1080);
    result.scale = scale_->currentIndex() + 2;
    result.width = width_->value(); result.height = height_->value(); result.lockRatio = lockRatio_->isChecked();
    result.fit = fit_->currentIndex() == 1 ? QStringLiteral("crop") : QStringLiteral("pad");
    result.model = superresModelIds().value(model_->currentIndex());
    result.reuseIdenticalFrames = reuseFrames_->isChecked();
    result.quality = QStringList{"balanced", "high", "small"}.value(quality_->currentIndex());
    result.codec = codec_->currentIndex() == 1 ? QStringLiteral("libx265") : QStringLiteral("libx264");
    result.audio = QStringList{"copy", "aac", "none"}.value(audio_->currentIndex());
    result.container = container_->currentIndex() == 1 ? QStringLiteral("mkv") : QStringLiteral("mp4");
    return result;
}

void SuperresInterface::probeVideo() {
    if (probe_->state() != QProcess::NotRunning) { probe_->kill(); probe_->waitForFinished(500); }
    if (!QFileInfo(inputPath()).isFile()) return;
    if (sourceVideoPath_ != inputPath()) {
        sourceVideoPath_ = inputPath();
        sourceFrames_->open(sourceVideoPath_);
    }
    sourceInfo_->setText(trText("正在读取视频信息…"));
    probe_->setProperty("inputPath", inputPath());
    const int generation = probe_->property("generation").toInt() + 1;
    probe_->setProperty("generation", generation);
    probe_->start(SuperresOptions::fromConfig().ffprobePath, superresProbeArguments(inputPath()));
    QTimer::singleShot(15000, probe_, [process = QPointer<QProcess>(probe_), generation]() {
        if (process && process->property("generation").toInt() == generation && process->state() != QProcess::NotRunning) process->kill();
    });
}

double SuperresInterface::previewStartSeconds() const {
    return videoInfo_.fps > 0 ? sourceSlider_->value() / videoInfo_.fps : 0;
}

void SuperresInterface::updatePreviewAvailability() {
    previewButton_->setEnabled(!previewWorker_ && probedPath_ == inputPath() &&
        !videoInfo_.size.isEmpty() && sourceCanvas_->hasFrame() && displayedSourceFrame_ == sourceSlider_->value());
}

void SuperresInterface::updateSourcePosition() {
    const int total = sourceFrames_->totalFrames();
    const int index = sourceSlider_->value();
    sourceFrameLabel_->setText(total > 0 ? QStringLiteral("%1/%2 %3").arg(index + 1).arg(total).arg(trText("帧")) : trText("帧：-/-"));
    const double fps = videoInfo_.fps > 0 ? videoInfo_.fps : sourceFrames_->fps();
    sourceTimeLabel_->setText(fps > 0 ? trText("起点：") + previewTimeText(index / fps) + QStringLiteral(" / ") +
        previewTimeText(sourceFrames_->duration()) : trText("起点：--:--.---"));
}

void SuperresInterface::refreshTarget() {
    const auto settings = options();
    presetCard_->setVisible(settings.sizeMode == QStringLiteral("preset"));
    scaleCard_->setVisible(settings.sizeMode == QStringLiteral("scale"));
    customRow_->setVisible(settings.sizeMode == QStringLiteral("custom"));
    fit_->parentWidget()->setVisible(settings.sizeMode == QStringLiteral("custom") && !settings.lockRatio);
    if (videoInfo_.size.isEmpty()) { targetInfo_->setText(trText("保持原始帧率与画面比例，输出文件另存。")); return; }
    const QSize target = settings.targetSize(videoInfo_.size);
    targetInfo_->setText(QStringLiteral("%1 × %2 → %3 × %4 · %5 fps · %6")
        .arg(videoInfo_.size.width()).arg(videoInfo_.size.height()).arg(target.width()).arg(target.height())
        .arg(videoInfo_.fps, 0, 'f', 3).arg(trText("保留原始帧率")));
    const QString generated = QFileInfo(inputPath()).dir().filePath(QFileInfo(inputPath()).completeBaseName() +
        QStringLiteral("_SR_%1x%2.").arg(target.width()).arg(target.height()) + settings.container);
    if (generatedOutput_.isEmpty() || outputPath() == generatedOutput_) outputFileCard_->lineEdit->setText(generated);
    generatedOutput_ = generated;
}

bool SuperresInterface::validateBeforeStart(QString* error) {
    if (probedPath_ != inputPath() || videoInfo_.size.isEmpty()) *error = trText("请先选择视频并等待读取视频信息。");
    else if (videoInfo_.variableFrameRate) *error = trText("此视频检测为可变帧率，请先在压制中转换为固定帧率。");
    else if (videoInfo_.hdr) *error = trText("当前超分支持 SDR 视频，请先将 HDR 视频转换为 SDR。");
    else if (QFileInfo::exists(outputPath())) *error = trText("输出文件已存在，请选择新的输出文件名。");
    else if (QFileInfo(outputPath()).suffix().toLower() != options().container) *error = trText("输出文件扩展名与所选格式不一致，请重新选择保存路径或输出格式。");
    else *error = options().validate(videoInfo_.size);
    return error->isEmpty();
}

void SuperresInterface::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1 && event->mimeData()->urls().first().isLocalFile()) event->acceptProposedAction();
}
void SuperresInterface::dropEvent(QDropEvent* event) {
    if (event->mimeData()->urls().size() == 1) { setInputPath(event->mimeData()->urls().first().toLocalFile()); event->acceptProposedAction(); }
}

void SuperresInterface::startPreview() {
    if (previewWorker_) return;
    QString error;
    if (probedPath_ != inputPath() || videoInfo_.size.isEmpty()) error = trText("请先选择视频并等待读取视频信息。");
    else if (videoInfo_.variableFrameRate) error = trText("此视频检测为可变帧率，请先在压制中转换为固定帧率。");
    else if (videoInfo_.hdr) error = trText("当前超分支持 SDR 视频，请先将 HDR 视频转换为 SDR。");
    else if (!sourceCanvas_->hasFrame() || displayedSourceFrame_ != sourceSlider_->value()) error = trText("请等待当前预览画面加载完成。");
    else error = options().validate(videoInfo_.size);
    if (!error.isEmpty()) { NotificationService::error(trText("无法开始预览"), error, this); return; }
    if (!previewPath_.isEmpty()) QFile::remove(previewPath_);
    QDir().mkpath(QDir(userDataFolder()).filePath(QStringLiteral("SuperresPreview")));
    previewPath_ = QDir(userDataFolder()).filePath(QStringLiteral("SuperresPreview/preview-%1.mp4").arg(QDateTime::currentMSecsSinceEpoch()));
    previewSource_ = inputPath();
    previewSourceStart_ = previewStartSeconds();
    previewOptions_ = options(); previewOptions_.container = QStringLiteral("mp4");
    auto task = std::make_shared<SuperresTask>(previewSource_, previewPath_, previewOptions_);
    task->preview = true; task->previewStart = previewSourceStart_;
    auto* worker = new SuperresWorker(task); previewWorker_ = worker;
    previewButton_->setEnabled(false); viewPreview_->hide(); cancelPreview_->show(); cancelPreview_->setEnabled(true);
    previewProgress_->setRange(0, 0); previewProgress_->show(); previewStatus_->setText(trText("等待处理资源…"));
    previewReuseInfo_->clear();
    connect(worker, &SuperresWorker::frameReuseProgress, this, [this](qint64, qint64 reused, qint64 unique, qint64 total) {
        previewReuseInfo_->setText(QStringLiteral("%1 %2 %3 · %4 %5 %3 · %6 %7 %3 · %8 %9 %3")
            .arg(trText("原始")).arg(total).arg(trText("帧")).arg(trText("去重后需超分")).arg(unique)
            .arg(trText("可复用")).arg(total - unique).arg(trText("已复用")).arg(reused));
    });
    connect(worker, &SuperresWorker::progress, this, [this](int percent, const QString& stage, qint64 frames, qint64 total, double fps, double elapsed, double remaining) {
        if (previewWorker_ && !cancelPreview_->isEnabled()) return;
        previewProgress_->setRange(0, total > 0 ? 100 : 0); previewProgress_->setValue(percent);
        if (stage.startsWith(trText("检测相同帧"))) {
            previewStatus_->setText(stage + QStringLiteral("\n%1 %2 · %3")
                .arg(trText("已用时")).arg(timeText(elapsed)).arg(trText("分析阶段不计入渲染速度")));
            return;
        }
        const QString counts = total > 0 ? QStringLiteral("%1 %2/%3 · %4 %5 %6")
            .arg(trText("已处理")).arg(frames).arg(total).arg(trText("剩余")).arg(qMax<qint64>(0, total - frames)).arg(trText("帧")) : QString();
        if (total > 0 && frames >= total) {
            previewStatus_->setText(stage + QStringLiteral("\n%1 · %2\n%3 %4 · %5")
                .arg(trText("超分已完成")).arg(counts).arg(trText("已用时")).arg(timeText(elapsed)).arg(trText("等待视频输出")));
            return;
        }
        previewStatus_->setText(stage + (total > 0 ? QStringLiteral(" · %1% · %2").arg(percent).arg(counts) : QString()) +
            QStringLiteral("\n%1 · %2 %3 · %4 %5").arg(fps > 0 ? QStringLiteral("%1 %2 %3").arg(trText("超分速度")).arg(fps, 0, 'f', 1).arg(trText("帧/秒")) : trText("超分速度估算中"))
                .arg(trText("已用时")).arg(timeText(elapsed)).arg(trText("超分预计剩余")).arg(remaining >= 0 ? timeText(remaining) : trText("估算中")));
    });
    connect(worker, &SuperresWorker::completed, this, [this](bool success, bool cancelled, const QString&, const QString& error) {
        previewWorker_ = nullptr; cancelPreview_->hide(); updatePreviewAvailability();
        previewProgress_->setRange(0, 100); previewProgress_->setValue(success ? 100 : 0);
        previewStatus_->setText(success ? trText("预览渲染完成，可以播放对比或逐帧查看。") : cancelled ? trText("预览已取消，临时文件已清理。") : trText("预览失败：") + error);
        previewButton_->setText(success || cancelled ? trText("预览 5 秒效果") : trText("重试预览"));
        viewPreview_->setVisible(success);
        if (success) showPreview();
    });
    emit previewRequested(worker);
}

void SuperresInterface::showPreview() {
    if (!QFileInfo::exists(previewPath_)) return;
    SuperresPreviewDialog dialog(previewSource_, previewPath_, previewSourceStart_, window()); dialog.exec();
}

SuperresTaskInterface::SuperresTaskInterface(QWidget* parent) : BaseTaskInterface(PreviewTaskKind::Superres, nullptr, parent) {}
void SuperresTaskInterface::enqueue(const QString& input, const QString& output, const SuperresOptions& options) {
    pendingOptions_ = options; addTask(input, output);
}
std::shared_ptr<TaskBase> SuperresTaskInterface::createTask(const QString& input, const QString& output) {
    return std::make_shared<SuperresTask>(input, output, pendingOptions_);
}
TaskWorker* SuperresTaskInterface::createWorker(const std::shared_ptr<TaskBase>& task) { return new SuperresWorker(std::static_pointer_cast<SuperresTask>(task)); }
QString SuperresTaskInterface::getTaskPath(const std::shared_ptr<TaskBase>& task) const { return QDir::cleanPath(QFileInfo(task->outputPath).absoluteFilePath()).toCaseFolded(); }
QString SuperresTaskInterface::taskTypeText() const { return trText("视频超分"); }
void SuperresTaskInterface::enqueuePreview(SuperresWorker* worker) {
    static int previewKey = -1;
    const int key = previewKey--;
    threadMap_.insert(key, worker);
    connect(worker, &SuperresWorker::completed, this, [this, key]() { threadMap_.remove(key); });
    taskPool_.start(worker, 1);
}
void SuperresTaskInterface::cancelPreview(SuperresWorker* worker) {
    worker->cancel();
    if (taskPool_.tryTake(worker)) worker->cancelQueued();
}

SuperresSettingInterface::SuperresSettingInterface(QWidget* parent) : AdvancedSettingInterface(trText("高级设置"), parent) {
    const auto initial = SuperresOptions::fromConfig();
    auto* paths = addGroup(trText("超分资源"));
    const auto pathCard = [this, paths](const QString& title, const QString& description, const QString& value, const char* key, bool directory) {
        auto* card = new ChooseFileSettingCard(qfw::FluentIconEnum::Folder, title, description, QString(), paths);
        card->lineEdit->setText(value); paths->addSettingCard(card);
        connect(card->browseBtn, &QPushButton::clicked, this, [this, card, key, directory]() {
            const QString path = directory ? QFileDialog::getExistingDirectory(this, trText("选择目录"), card->lineEdit->text())
                : QFileDialog::getOpenFileName(this, trText("选择文件"), card->lineEdit->text());
            if (!path.isEmpty()) { card->lineEdit->setText(path); save(key, path); }
        });
    };
    pathCard(trText("超分引擎"), QStringLiteral("Real-ESRGAN ncnn Vulkan"), initial.enginePath, "EnginePath", false);
    pathCard(trText("模型目录"), trText("选择包含 .param 和 .bin 的 models 文件夹"), initial.modelsPath, "ModelsPath", true);
    pathCard(QStringLiteral("ffprobe"), trText("用于读取视频信息与校验输出"), initial.ffprobePath, "FFprobePath", false);
    pathCard(trText("临时目录"), trText("按批次缓存，任务结束或取消后自动清理"), initial.tempPath, "TempPath", true);
    auto* performance = addGroup(trText("性能与显存"));
    integerCard(performance, trText("显卡编号"), trText("-1 自动选择；其他编号与引擎列出的设备一致"), initial.gpu, -1, 15, "Gpu");
    integerCard(performance, trText("分块大小"), trText("0 自动；手动值至少为 32，减小可降低显存占用"), initial.tile, 0, 4096, "Tile");
    integerCard(performance, trText("每批缓存帧数"), trText("数值越小，临时图片占用越少；任务与预览共用串行队列"), initial.batchFrames, 1, 120, "BatchFrames");
    auto* tta = new qfw::SwitchSettingCard(qfw::FluentIconEnum::Setting, trText("增强处理（TTA）"), trText("通过多次推理融合结果，处理时间会增加"), nullptr, performance);
    tta->setChecked(initial.tta);
    connect(tta, &qfw::SwitchSettingCard::checkedChanged, this, [](bool checked) { save("Tta", checked); });
    performance->addSettingCard(tta);
    auto* resources = addGroup(trText("资源说明"));
    auto* link = new qfw::PushSettingCard(trText("下载页面"), qfw::FluentIconEnum::Download, trText("Real-ESRGAN 官方资源"), trText("需要支持 Vulkan 的显卡；解压后选择引擎及模型目录"), resources);
    connect(link, &qfw::PushSettingCard::clicked, this, []() { QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0"))); });
    resources->addSettingCard(link); finish();
}

SuperresStackedInterfaces::SuperresStackedInterfaces(QWidget* parent) : BaseStackedInterfaces(parent) {
    setObjectName(QStringLiteral("SuperresStackedInterfaces"));
    auto* main = new SuperresInterface(this); auto* tasks = new SuperresTaskInterface(this);
    addSubInterface(main, QStringLiteral("mainInterface"), trText("视频超分"));
    addSubInterface(tasks, QStringLiteral("taskInterface"), trText("视频超分任务"));
    addSubInterface(new SuperresSettingInterface(this), QStringLiteral("settingInterface"), trText("高级设置"));
    connect(main, &BaseFunctionInterface::taskRequested, tasks, [main, tasks](const QString& input, const QString& output) { tasks->enqueue(input, output, main->options()); });
    connect(tasks, &BaseTaskInterface::returnTask, main, &BaseFunctionInterface::updateTask);
    connect(main, &SuperresInterface::previewRequested, tasks, &SuperresTaskInterface::enqueuePreview);
    connect(main, &SuperresInterface::cancelPreviewRequested, tasks, &SuperresTaskInterface::cancelPreview);
}
} // namespace fkw
