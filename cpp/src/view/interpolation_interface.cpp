#include "view/interpolation_interface.h"

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
class InlineInterpolationCard : public qfw::SettingCard {
public:
    using qfw::SettingCard::SettingCard;
    QHBoxLayout* row() { return hBoxLayout_; }
};
void save(const char* key, const QJsonValue& value) {
    AppConfig::instance().set(QStringLiteral("Interpolation"), QString::fromLatin1(key), value);
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
            if (QString::fromLatin1(key) == QStringLiteral("Multiplier")) save(key, values.at(index).toInt());
            else save(key, values.at(index));
        }
    });
    group->addSettingCard(card);
    if (cardOut) *cardOut = card;
    return card->comboBox;
}
qfw::SpinBox* integerCard(qfw::SettingCardGroup* group, const QString& title, const QString& description,
                         int value, int minimum, int maximum, const char* key) {
    auto* card = new InlineInterpolationCard(qfw::FluentIconEnum::Setting, title, description, group);
    auto* spin = new qfw::SpinBox(card);
    spin->setRange(minimum, maximum); spin->setValue(value); spin->setMinimumWidth(150);
    card->row()->addWidget(spin); card->row()->addSpacing(16);
    QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), card, [key](int n) { save(key, n); });
    group->addSettingCard(card);
    return spin;
}

// Compare the same playback time at the two different frame rates.
class InterpolationPreviewDialog : public QDialog {
public:
    InterpolationPreviewDialog(const QString& source, const QString& result, double start, double sourceRate, QWidget* parent)
        : QDialog(parent), source_(source.toStdString()), result_(result.toStdString()) {
        setWindowTitle(trText("视频补帧预览")); resize(1000, 650);
        auto* layout = new QVBoxLayout(this);
        auto* headings = new QHBoxLayout;
        headings->addWidget(new qfw::BodyLabel(trText("原视频"), this), 1);
        headings->addWidget(new qfw::BodyLabel(trText("补帧结果"), this), 1);
        layout->addLayout(headings);
        auto* row = new QHBoxLayout;
        before_ = new QLabel(this); after_ = new QLabel(this);
        before_->setObjectName("interpolationComparisonSource");
        for (auto* label : {before_, after_}) { label->setAlignment(Qt::AlignCenter); label->setMinimumSize(160, 120); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored); row->addWidget(label, 1); }
        layout->addLayout(row, 1);
        slider_ = new qfw::Slider(Qt::Horizontal, this);
        slider_->setObjectName("interpolationComparisonSlider");
        const int frames = int(result_.get(cv::CAP_PROP_FRAME_COUNT));
        fps_ = result_.get(cv::CAP_PROP_FPS);
        if (fps_ <= 0) fps_ = 30;
        sourceFps_ = sourceRate;
        if (sourceFps_ <= 0) sourceFps_ = fps_;
        offset_ = qRound64(start * sourceFps_);
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
            label->setProperty("frameIndex", index);
            if (qRound64(capture.get(cv::CAP_PROP_POS_FRAMES)) != index) capture.set(cv::CAP_PROP_POS_FRAMES, double(index));
            cv::Mat image;
            if (!capture.read(image) || image.empty() || image.type() != CV_8UC3) { label->setText(trText("无法读取预览帧")); return; }
            const QImage picture(image.data, image.cols, image.rows, int(image.step), QImage::Format_BGR888);
            label->setPixmap(QPixmap::fromImage(picture).scaled(label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        };
        show(source_, qMin<qint64>(qint64(source_.get(cv::CAP_PROP_FRAME_COUNT)) - 1, offset_ + qint64(std::floor(frame * sourceFps_ / fps_ + 1e-8))), before_); show(result_, frame, after_);
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
    double fps_ = 30, sourceFps_ = 30;
};
}

InterpolationInterface::InterpolationInterface(QWidget* parent)
    : BaseFunctionInterface(trText("补帧"), qfw::FluentIconEnum::Video, parent), probe_(new QProcess(this)) {
    setObjectName(QStringLiteral("interpolationInterface"));
    setOutputSuffix(QStringLiteral("_FPS.mp4"));
    setFileFilter(QStringLiteral("*.mp4;*.mkv;*.mov;*.avi;*.webm;*.flv;*.m4v;*.ts"));
    const auto initial = InterpolationOptions::fromConfig();
    auto* sourceCard = new InlineInterpolationCard(qfw::FluentIconEnum::Info, trText("视频信息"), QString(), fileSelectionGroup_);
    sourceInfo_ = new qfw::BodyLabel(trText("选择视频后读取尺寸、帧率、时长和音轨"), sourceCard);
    sourceInfo_->setObjectName(QStringLiteral("interpolationSourceInfo"));
    sourceInfo_->setWordWrap(true); sourceCard->row()->addWidget(sourceInfo_, 1); sourceCard->row()->addSpacing(16);
    fileSelectionGroup_->addSettingCard(sourceCard);
    rateMode_ = choice(settingsGroup_, trText("帧率设置"), trText("保留视频时长，提高播放帧率"),
        {trText("常用帧率"), trText("按倍数提高"), trText("自定义帧率")}, {"preset", "multiply", "custom"}, initial.rateMode, "RateMode");
    rateMode_->setObjectName("interpolationRateMode");
    preset_ = choice(settingsGroup_, trText("目标帧率"), trText("59.94 / 119.88 适合 29.97 fps 素材"),
        {"60 fps", "59.94 fps (60000/1001)", "120 fps", "119.88 fps (120000/1001)", "50 fps", "30 fps"},
        {"60/1", "60000/1001", "120/1", "120000/1001", "50/1", "30/1"}, initial.presetRate, "PresetRate", &presetCard_);
    preset_->setObjectName("interpolationPresetRate");
    multiplier_ = choice(settingsGroup_, trText("补帧倍数"), trText("按原始帧率精确提高，保留小数帧率"),
        {trText("2 倍"), trText("3 倍"), trText("4 倍")}, {"2", "3", "4"}, QString::number(initial.multiplier), "Multiplier", &multiplierCard_);
    auto* custom = new InlineInterpolationCard(qfw::FluentIconEnum::Video, trText("自定义目标帧率"), trText("1–240 fps，需要高于原视频帧率"), settingsGroup_);
    customCard_ = custom; customRate_ = new qfw::SpinBox(custom); customRate_->setRange(1, 240); customRate_->setValue(initial.customRate);
    customRate_->setMinimumWidth(150); customRate_->setObjectName("interpolationCustomRate");
    custom->row()->addWidget(customRate_); custom->row()->addSpacing(16); settingsGroup_->addSettingCard(custom);
    model_ = choice(settingsGroup_, trText("补帧模型"), trText("用于东方 MMD；先预览转身、手部动作、快速运镜与切镜"),
        {trText("RIFE 4.6（默认）"), trText("RIFE 4.0（对比）")}, {"rife-v4.6", "rife-v4"}, initial.model, "Model");
    model_->setObjectName("interpolationModel");
    reuseHolds_ = new qfw::SwitchSettingCard(qfw::FluentIconEnum::Sync, trText("静止画面复用"),
        trText("相邻画面完全相同时直接复用，避免反复计算相同画面"), nullptr, settingsGroup_);
    reuseHolds_->setChecked(initial.reuseHolds); reuseHolds_->setObjectName("interpolationReuseHolds"); settingsGroup_->addSettingCard(reuseHolds_);
    protectCuts_ = new qfw::SwitchSettingCard(qfw::FluentIconEnum::Video, trText("切镜保护"),
        trText("检测到镜头切换时保持原画面，避免跨镜头补帧产生变形"), nullptr, settingsGroup_);
    protectCuts_->setChecked(initial.protectCuts); protectCuts_->setObjectName("interpolationProtectCuts"); settingsGroup_->addSettingCard(protectCuts_);
    quality_ = choice(settingsGroup_, trText("编码画质"), trText("影响结果的压缩质量与文件大小"), {trText("均衡"), trText("高画质"), trText("小体积")},
        {"balanced", "high", "small"}, initial.quality, "Quality");
    codec_ = choice(settingsGroup_, trText("视频编码"), QString(), {"H.264", "H.265"}, {"libx264", "libx265"}, initial.codec, "Codec");
    container_ = choice(settingsGroup_, trText("输出格式"), QString(), {"MP4", "MKV"}, {"mp4", "mkv"}, initial.container, "Container");
    audio_ = choice(settingsGroup_, trText("音频处理"), trText("保持音画同步；音频格式不兼容时可选 AAC"),
        {trText("保留原音频"), trText("转为 AAC"), trText("移除音频")}, {"copy", "aac", "none"}, initial.audio, "Audio");
    targetInfo_ = new qfw::BodyLabel(this); targetInfo_->setWordWrap(true); targetInfo_->setObjectName("interpolationTargetInfo");
    mainLayout_->insertWidget(mainLayout_->count() - 2, targetInfo_);

    auto* preview = new qfw::SimpleCardWidget(this);
    auto* previewLayout = new QVBoxLayout(preview);
    previewLayout->setContentsMargins(20, 20, 20, 20);
    previewLayout->addWidget(new qfw::StrongBodyLabel(trText("视频预览"), preview));
    sourceCanvas_ = new PreviewCanvas(preview);
    sourceCanvas_->setObjectName(QStringLiteral("interpolationSourcePreview"));
    previewLayout->addWidget(sourceCanvas_);
    auto* position = new QHBoxLayout;
    sourceSlider_ = new qfw::Slider(Qt::Horizontal, preview);
    sourceSlider_->setObjectName(QStringLiteral("interpolationSourceSlider"));
    sourceSlider_->setTracking(true); sourceSlider_->setEnabled(false);
    sourceFrameLabel_ = new qfw::CaptionLabel(trText("帧：-/-"), preview);
    sourceTimeLabel_ = new qfw::CaptionLabel(trText("起点：--:--.---"), preview);
    sourceTimeLabel_->setObjectName(QStringLiteral("interpolationSourceTime"));
    position->addWidget(sourceSlider_, 4);
    position->addWidget(sourceFrameLabel_);
    position->addWidget(sourceTimeLabel_);
    previewLayout->addLayout(position);
    auto* hint = new qfw::BodyLabel(trText("拖动进度条选择起点，从当前画面开始预览最多 5 秒。"), preview);
    hint->setWordWrap(true); previewLayout->addWidget(hint);
    auto* controls = new QHBoxLayout;
    previewButton_ = new qfw::PushButton(trText("预览 5 秒效果"), preview); previewButton_->setEnabled(false);
    previewButton_->setObjectName(QStringLiteral("interpolationPreviewButton"));
    cancelPreview_ = new qfw::PushButton(trText("取消预览"), preview); cancelPreview_->hide();
    viewPreview_ = new qfw::PushButton(trText("查看预览对比"), preview); viewPreview_->hide();
    controls->addWidget(previewButton_); controls->addWidget(cancelPreview_); controls->addWidget(viewPreview_); controls->addStretch();
    previewLayout->addLayout(controls);
    previewProgress_ = new qfw::ProgressBar(preview, false); previewProgress_->hide();
    previewProgress_->setObjectName(QStringLiteral("interpolationPreviewProgress"));
    previewStatus_ = new qfw::BodyLabel(preview); previewStatus_->setWordWrap(true);
    previewStatus_->setObjectName(QStringLiteral("interpolationPreviewStatus"));
    previewLayout->addWidget(previewProgress_); previewLayout->addWidget(previewStatus_);
    previewStats_ = new qfw::CaptionLabel(preview);
    previewStats_->setObjectName(QStringLiteral("interpolationPreviewStats")); previewStats_->setWordWrap(true);
    previewLayout->addWidget(previewStats_);
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
    connect(&probeDelay_, &QTimer::timeout, this, &InterpolationInterface::probeVideo);
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
        videoInfo_ = code == 0 && status == QProcess::NormalExit ? parseInterpolationVideoInfo(QJsonDocument::fromJson(probe_->readAllStandardOutput()).object()) : SuperresVideoInfo{};
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
    for (auto* combo : {rateMode_, preset_, multiplier_, model_, quality_, codec_, audio_, container_})
        connect(combo, &qfw::ComboBox::currentIndexChanged, this, [this](int) { refreshTarget(); });
    connect(customRate_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) { save("CustomRate", value); refreshTarget(); });
    connect(reuseHolds_, &qfw::SwitchSettingCard::checkedChanged, this, [](bool checked) { save("ReuseHolds", checked); });
    connect(protectCuts_, &qfw::SwitchSettingCard::checkedChanged, this, [](bool checked) { save("ProtectCuts", checked); });
    connect(previewButton_, &QPushButton::clicked, this, &InterpolationInterface::startPreview);
    connect(cancelPreview_, &QPushButton::clicked, this, [this]() {
        cancelPreview_->setEnabled(false); previewStatus_->setText(trText("正在取消预览…"));
        if (previewWorker_) emit cancelPreviewRequested(previewWorker_);
    });
    connect(viewPreview_, &QPushButton::clicked, this, &InterpolationInterface::showPreview);
    connect(&AppConfig::instance(), &AppConfig::valueChanged, this, [this](const QString& group, const QString& key, const QJsonValue&) {
        if (group == "Interpolation" && key == "FFprobePath" && !inputPath().isEmpty()) probeDelay_.start();
    });
    refreshTarget();
}
InterpolationInterface::~InterpolationInterface() {
    if (previewWorker_) emit cancelPreviewRequested(previewWorker_);
    if (probe_->state() != QProcess::NotRunning) { probe_->kill(); probe_->waitForFinished(500); }
    if (!previewPath_.isEmpty()) QFile::remove(previewPath_);
}
InterpolationOptions InterpolationInterface::options() const {
    auto result = InterpolationOptions::fromConfig();
    result.rateMode = QStringList{"preset", "multiply", "custom"}.value(rateMode_->currentIndex());
    result.presetRate = QStringList{"60/1", "60000/1001", "120/1", "120000/1001", "50/1", "30/1"}.value(preset_->currentIndex());
    result.multiplier = multiplier_->currentIndex() + 2; result.customRate = customRate_->value();
    result.model = model_->currentIndex() == 0 ? "rife-v4.6" : "rife-v4";
    result.reuseHolds = reuseHolds_->isChecked(); result.protectCuts = protectCuts_->isChecked();
    result.quality = QStringList{"balanced", "high", "small"}.value(quality_->currentIndex());
    result.codec = codec_->currentIndex() == 0 ? "libx264" : "libx265";
    result.audio = QStringList{"copy", "aac", "none"}.value(audio_->currentIndex());
    result.container = container_->currentIndex() == 0 ? "mp4" : "mkv"; return result;
}

void InterpolationInterface::probeVideo() {
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
    probe_->start(InterpolationOptions::fromConfig().ffprobePath, superresProbeArguments(inputPath()));
    QTimer::singleShot(15000, probe_, [process = QPointer<QProcess>(probe_), generation]() {
        if (process && process->property("generation").toInt() == generation && process->state() != QProcess::NotRunning) process->kill();
    });
}

double InterpolationInterface::previewStartSeconds() const {
    return videoInfo_.fps > 0 ? sourceSlider_->value() / videoInfo_.fps : 0;
}

void InterpolationInterface::updatePreviewAvailability() {
    previewButton_->setEnabled(!previewWorker_ && probedPath_ == inputPath() &&
        !videoInfo_.size.isEmpty() && sourceCanvas_->hasFrame() && displayedSourceFrame_ == sourceSlider_->value());
}

void InterpolationInterface::updateSourcePosition() {
    const int total = sourceFrames_->totalFrames();
    const int index = sourceSlider_->value();
    sourceFrameLabel_->setText(total > 0 ? QStringLiteral("%1/%2 %3").arg(index + 1).arg(total).arg(trText("帧")) : trText("帧：-/-"));
    const double fps = videoInfo_.fps > 0 ? videoInfo_.fps : sourceFrames_->fps();
    sourceTimeLabel_->setText(fps > 0 ? trText("起点：") + previewTimeText(index / fps) + QStringLiteral(" / ") +
        previewTimeText(sourceFrames_->duration()) : trText("起点：--:--.---"));
}

void InterpolationInterface::refreshTarget() {
    const auto settings = options();
    presetCard_->setVisible(settings.rateMode == "preset"); multiplierCard_->setVisible(settings.rateMode == "multiply"); customCard_->setVisible(settings.rateMode == "custom");
    if (videoInfo_.size.isEmpty()) { targetInfo_->setText(trText("保持画面尺寸与视频时长，输出文件另存。")); return; }
    const double target = interpolationRate(settings.targetRate(videoInfo_.frameRate));
    targetInfo_->setText(QStringLiteral("%1 × %2 · %3 → %4 fps · %5").arg(videoInfo_.size.width()).arg(videoInfo_.size.height())
        .arg(videoInfo_.fps, 0, 'f', 3).arg(target, 0, 'f', 3).arg(trText("保持原时长与画面比例")));
    const QString generated = QFileInfo(inputPath()).dir().filePath(QFileInfo(inputPath()).completeBaseName() +
        QStringLiteral("_FPS_%1.").arg(target, 0, 'g', 6) + settings.container);
    if (generatedOutput_.isEmpty() || outputPath() == generatedOutput_) outputFileCard_->lineEdit->setText(generated);
    generatedOutput_ = generated;
}
bool InterpolationInterface::validateBeforeStart(QString* error) {
    if (probedPath_ != inputPath() || videoInfo_.size.isEmpty()) *error = trText("请先选择视频并等待读取视频信息。");
    else if (videoInfo_.variableFrameRate || videoInfo_.hdr) *error = trText("当前补帧支持固定帧率 SDR 视频，请先在压制中转换。");
    else if (QFileInfo::exists(outputPath())) *error = trText("输出文件已存在，请选择新的输出文件名。");
    else if (QFileInfo(outputPath()).suffix().toLower() != options().container) *error = trText("输出扩展名与所选格式不一致。");
    else *error = options().validate(videoInfo_);
    return error->isEmpty();
}
void InterpolationInterface::startPreview() {
    if (previewWorker_) return;
    QString error;
    if (probedPath_ != inputPath() || videoInfo_.size.isEmpty()) error = trText("请先选择视频并等待读取视频信息。");
    else if (videoInfo_.variableFrameRate || videoInfo_.hdr) error = trText("当前补帧支持固定帧率 SDR 视频，请先在压制中转换。");
    else if (!sourceCanvas_->hasFrame() || displayedSourceFrame_ != sourceSlider_->value()) error = trText("请等待当前预览画面加载完成。");
    else error = options().validate(videoInfo_);
    if (!error.isEmpty()) { NotificationService::error(trText("无法开始预览"), error, this); return; }
    if (!previewPath_.isEmpty()) QFile::remove(previewPath_);
    QDir().mkpath(QDir(userDataFolder()).filePath("InterpolationPreview"));
    previewPath_ = QDir(userDataFolder()).filePath(QStringLiteral("InterpolationPreview/preview-%1.mp4").arg(QDateTime::currentMSecsSinceEpoch()));
    previewSource_ = inputPath(); previewSourceStart_ = previewStartSeconds(); previewSourceFps_ = videoInfo_.fps; previewOptions_ = options(); previewOptions_.container = "mp4";
    auto task = std::make_shared<InterpolationTask>(previewSource_, previewPath_, previewOptions_); task->preview = true; task->previewStart = previewSourceStart_;
    auto* worker = new InterpolationWorker(task); previewWorker_ = worker;
    previewButton_->setEnabled(false); viewPreview_->hide(); cancelPreview_->show(); cancelPreview_->setEnabled(true);
    previewProgress_->setRange(0, 0); previewProgress_->show(); previewStatus_->setText(trText("等待处理资源…")); previewStats_->clear();
    connect(worker, &InterpolationWorker::statistics, this, [this](qint64 source, qint64 output, qint64 work, qint64 holds, qint64 cuts) {
        previewStats_->setText(QStringLiteral("%1 %2 → %3 %4 · %5 %6 %4 · %7 %8 %4 · %9 %10 %4")
            .arg(trText("原始")).arg(source).arg(output).arg(trText("帧")).arg(trText("需计算新帧")).arg(work)
            .arg(trText("静止复用")).arg(holds).arg(trText("切镜保护")).arg(cuts));
    });
    connect(worker, &InterpolationWorker::progress, this, [this](int percent, const QString& stage, qint64 frames, qint64 total, double fps, double elapsed, double remaining) {
        if (previewWorker_ && !cancelPreview_->isEnabled()) return;
        previewProgress_->setRange(0, percent > 0 || total > 0 ? 100 : 0); previewProgress_->setValue(percent);
        if (stage.startsWith(trText("分析画面"))) {
            previewStatus_->setText(stage + QStringLiteral("\n%1 %2 · %3").arg(trText("已用时")).arg(timeText(elapsed)).arg(trText("分析阶段不计入补帧速度"))); return;
        }
        const QString counts = total > 0 ? QStringLiteral("%1 %2/%3 · %4 %5 %6")
            .arg(trText("已处理")).arg(frames).arg(total).arg(trText("剩余")).arg(qMax<qint64>(0, total - frames)).arg(trText("帧")) : QString();
        if ((total > 0 && frames >= total) || (total == 0 && percent >= 99)) {
            previewStatus_->setText(stage + QStringLiteral("\n%1 · %2\n%3 %4")
                .arg(total > 0 ? trText("补帧计算已完成") : trText("无需模型补帧，复用原始画面"))
                .arg(counts).arg(trText("已用时")).arg(timeText(elapsed))); return;
        }
        previewStatus_->setText(stage + (total > 0 ? QStringLiteral(" · %1% · %2").arg(percent).arg(counts) : QString()) +
            QStringLiteral("\n%1 · %2 %3 · %4 %5")
                .arg(fps > 0 ? QStringLiteral("%1 %2 %3").arg(trText("补帧速度")).arg(fps, 0, 'f', 1).arg(trText("帧/秒")) : trText("补帧速度估算中"))
                .arg(trText("已用时")).arg(timeText(elapsed)).arg(trText("补帧预计剩余")).arg(remaining >= 0 ? timeText(remaining) : trText("估算中")));
    });
    connect(worker, &InterpolationWorker::completed, this, [this](bool success, bool cancelled, const QString&, const QString& error) {
        previewWorker_ = nullptr; cancelPreview_->hide(); updatePreviewAvailability();
        previewProgress_->setRange(0, 100); previewProgress_->setValue(success ? 100 : 0);
        previewStatus_->setText(success ? trText("预览渲染完成，可以按相同播放时间对比或逐帧查看。") : cancelled ? trText("预览已取消，临时文件已清理。") : trText("预览失败：") + error);
        previewButton_->setText(success || cancelled ? trText("预览 5 秒效果") : trText("重试预览")); viewPreview_->setVisible(success);
        if (success) showPreview();
    });
    emit previewRequested(worker);
}
void InterpolationInterface::showPreview() {
    if (!QFileInfo::exists(previewPath_)) return;
    InterpolationPreviewDialog dialog(previewSource_, previewPath_, previewSourceStart_, previewSourceFps_, window()); dialog.exec();
}

void InterpolationInterface::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1 && event->mimeData()->urls().first().isLocalFile()) event->acceptProposedAction();
}
void InterpolationInterface::dropEvent(QDropEvent* event) {
    if (event->mimeData()->urls().size() == 1) { setInputPath(event->mimeData()->urls().first().toLocalFile()); event->acceptProposedAction(); }
}

InterpolationTaskInterface::InterpolationTaskInterface(QWidget* parent) : BaseTaskInterface(PreviewTaskKind::Interpolation, nullptr, parent) {}
void InterpolationTaskInterface::enqueue(const QString& input, const QString& output, const InterpolationOptions& options) {
    pendingOptions_ = options; addTask(input, output);
}
std::shared_ptr<TaskBase> InterpolationTaskInterface::createTask(const QString& input, const QString& output) {
    return std::make_shared<InterpolationTask>(input, output, pendingOptions_);
}
TaskWorker* InterpolationTaskInterface::createWorker(const std::shared_ptr<TaskBase>& task) { return new InterpolationWorker(std::static_pointer_cast<InterpolationTask>(task)); }
QString InterpolationTaskInterface::getTaskPath(const std::shared_ptr<TaskBase>& task) const { return QDir::cleanPath(QFileInfo(task->outputPath).absoluteFilePath()).toCaseFolded(); }
QString InterpolationTaskInterface::taskTypeText() const { return trText("视频补帧"); }
void InterpolationTaskInterface::enqueuePreview(InterpolationWorker* worker) {
    static int previewKey = -1;
    const int key = previewKey--;
    threadMap_.insert(key, worker);
    connect(worker, &InterpolationWorker::completed, this, [this, key]() { threadMap_.remove(key); });
    taskPool_.start(worker, 1);
}
void InterpolationTaskInterface::cancelPreview(InterpolationWorker* worker) {
    worker->cancel();
    if (taskPool_.tryTake(worker)) worker->cancelQueued();
}

InterpolationSettingInterface::InterpolationSettingInterface(QWidget* parent) : AdvancedSettingInterface(trText("高级设置"), parent) {
    const auto initial = InterpolationOptions::fromConfig(); auto* paths = addGroup(trText("补帧资源"));
    const auto pathCard = [this, paths](const QString& title, const QString& description, const QString& value, const char* key, bool directory) {
        auto* card = new ChooseFileSettingCard(qfw::FluentIconEnum::Folder, title, description, QString(), paths);
        card->lineEdit->setText(value); paths->addSettingCard(card);
        connect(card->lineEdit, &QLineEdit::editingFinished, card, [card, key]() { save(key, card->lineEdit->text()); });
        connect(card->browseBtn, &QPushButton::clicked, this, [this, card, key, directory]() {
            const QString path = directory ? QFileDialog::getExistingDirectory(this, trText("选择目录"), card->lineEdit->text())
                : QFileDialog::getOpenFileName(this, trText("选择文件"), card->lineEdit->text());
            if (!path.isEmpty()) { card->lineEdit->setText(path); save(key, path); }
        });
    };
    pathCard(trText("补帧引擎"), trText("随软件提供的 fkw-rife-bridge，整项任务持续加载模型"), initial.enginePath, "EnginePath", false);
    pathCard(trText("模型目录"), trText("选择含 rife-v4.6 / rife-v4 子目录的 models 文件夹"), initial.modelsPath, "ModelsPath", true);
    pathCard("ffprobe", trText("用于读取视频信息与校验输出"), initial.ffprobePath, "FFprobePath", false);
    pathCard(trText("临时目录"), trText("按批次缓存，任务结束或取消后自动清理"), initial.tempPath, "TempPath", true);
    auto* performance = addGroup(trText("性能与显存"));
    integerCard(performance, trText("显卡编号"), trText("-1 自动选择；其他编号与引擎日志中的设备一致"), initial.gpu, -1, 15, "Gpu");
    integerCard(performance, trText("每批缓存帧数"), trText("按原始帧计；预读、补帧与编码并行，较小值减少临时图片占用"), initial.batchFrames, 1, 120, "BatchFrames");
    const auto toggle = [performance](const QString& title, const QString& description, bool checked, const char* key) {
        auto* card = new qfw::SwitchSettingCard(qfw::FluentIconEnum::Setting, title, description, nullptr, performance);
        card->setChecked(checked); QObject::connect(card, &qfw::SwitchSettingCard::checkedChanged, card, [key](bool state) { save(key, state); }); performance->addSettingCard(card);
    };
    toggle(trText("高分辨率模式"), trText("降低运动估计的分辨率，保持输出尺寸；适合显存紧张时预览比较"), initial.uhd, "Uhd");
    toggle(trText("空间增强（TTA）"), trText("多次推理融合，处理时间会增加"), initial.spatialTta, "SpatialTta");
    toggle(trText("时间增强（TTA）"), trText("正反方向推理融合，处理时间会增加"), initial.temporalTta, "TemporalTta");
    auto* protection = addGroup(trText("东方 MMD 画面保护"));
    integerCard(protection, trText("切镜差异阈值（%）"), trText("画面差异达到阈值后，会匹配移动的画面细节。阈值越低，越容易触发切镜保护"), initial.sceneThreshold, 5, 80, "SceneThreshold");
    auto* resources = addGroup(trText("资源说明"));
    auto* link = new qfw::PushSettingCard(trText("下载页面"), qfw::FluentIconEnum::Download, trText("RIFE 官方模型资源"),
        trText("需要支持 Vulkan 的显卡；补帧与超分共用显卡排队，避免显存冲突"), resources);
    connect(link, &qfw::PushSettingCard::clicked, this, []() { QDesktopServices::openUrl(QUrl("https://github.com/nihui/rife-ncnn-vulkan/releases/tag/20221029")); });
    resources->addSettingCard(link); finish();
}

InterpolationStackedInterfaces::InterpolationStackedInterfaces(QWidget* parent) : BaseStackedInterfaces(parent) {
    setObjectName(QStringLiteral("InterpolationStackedInterfaces"));
    auto* main = new InterpolationInterface(this); auto* tasks = new InterpolationTaskInterface(this);
    addSubInterface(main, QStringLiteral("mainInterface"), trText("视频补帧"));
    addSubInterface(tasks, QStringLiteral("taskInterface"), trText("视频补帧任务"));
    addSubInterface(new InterpolationSettingInterface(this), QStringLiteral("settingInterface"), trText("高级设置"));
    connect(main, &BaseFunctionInterface::taskRequested, tasks, [main, tasks](const QString& input, const QString& output) { tasks->enqueue(input, output, main->options()); });
    connect(tasks, &BaseTaskInterface::returnTask, main, &BaseFunctionInterface::updateTask);
    connect(main, &InterpolationInterface::previewRequested, tasks, &InterpolationTaskInterface::enqueuePreview);
    connect(main, &InterpolationInterface::cancelPreviewRequested, tasks, &InterpolationTaskInterface::cancelPreview);
}
} // namespace fkw
