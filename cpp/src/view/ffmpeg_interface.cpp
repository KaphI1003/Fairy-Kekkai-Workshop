#include "view/ffmpeg_interface.h"
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include "view/ffmpeg_task_interface.h"
#include "common/app_data.h"
#include "common/config_keys.h"
#include "common/event_bus.h"
#include "common/text_format.h"

namespace fkw {
FFmpegInterface::FFmpegInterface(QWidget* parent)
    : BaseFunctionInterface(trText("压制"), qfw::FluentIconEnum::Video, parent) {
    setObjectName(QStringLiteral("ffmpegInterface"));
    setOutputSuffix(QStringLiteral("_compressed.mp4"));
    setFileFilter(QStringLiteral("*.mp4;*.flv;*.mkv;*.avi;*.wmv;*.mpg;*.avs;*.mov;*.webm;*.m4v;*.ts;*.m2ts"));
    setSpecialFilenameMapping(
        {{QStringLiteral("熟肉.mp4"), QStringLiteral("熟肉_compressed.mp4")}});
    subtitles_ = new ChooseFileSettingCard(qfw::FluentIconEnum::Document, trText("烧录字幕"),
        trText("自动匹配同名或项目译文。ASS / SSA 特效使用 VSFilterMod。"),
        trText("无字幕 · 可手动选择 ASS、SSA 或 SRT"), fileSelectionGroup_);
    subtitles_->lineEdit->setReadOnly(false);
    subtitles_->lineEdit->setClearButtonEnabled(true);
    fileSelectionGroup_->addSettingCard(subtitles_);
    connect(subtitles_->browseBtn, &QPushButton::clicked, this, [this] {
        const QString selected = QFileDialog::getOpenFileName(this, trText("选择字幕"),
            QFileInfo(inputPath()).absolutePath(), trText("字幕文件 (*.ass *.ssa *.srt)"));
        if (!selected.isEmpty()) subtitles_->lineEdit->setText(selected);
    });
    auto* gpu = boundSwitch(this, settingsGroup_, ConfigKeys::ffmpegGpuEncoding,
        qfw::FluentIconEnum::SpeedHigh, trText("GPU 加速编码（NVIDIA NVENC）"),
        trText("默认推荐：H.265 + CRF 21（NVENC 自动映射 CQ + temporal-aq），开启 GPU 可提速 4–5 倍但成片体积偏大；追求更小体积请关闭 GPU 或在高级设置开启 2-Pass VBR。"));
    gpu->setObjectName(QStringLiteral("ffmpegGpuEncoding"));
    fileSelectionGroup_->adjustSize();
    fileSelectionGroup_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
}

void FFmpegInterface::setInputPath(const QString& path) {
    BaseFunctionInterface::setInputPath(path);
    bool ambiguous = false;
    subtitles_->lineEdit->setText(findEncodeSubtitle(path, &ambiguous));
    subtitles_->lineEdit->setPlaceholderText(ambiguous ? trText("匹配到多个字幕，请手动选择；留空仅压制视频")
        : trText("无字幕 · 可手动选择 ASS、SSA 或 SRT"));
}

QString FFmpegInterface::subtitlePath() const {
    return subtitles_->lineEdit->text().trimmed();
}

bool FFmpegInterface::validateBeforeStart(QString* error) {
    if (!subtitlePath().isEmpty() && !QFileInfo(subtitlePath()).isFile()) {
        *error = trText("找不到所选字幕文件。");
        return false;
    }
    return true;
}

FFmpegStackedInterfaces::FFmpegStackedInterfaces(QWidget* parent)
    : BaseStackedInterfaces(parent) {
    setObjectName(QStringLiteral("FFmpegStackedInterfaces"));
    auto* main = new FFmpegInterface(this);
    auto* tasks = new FFmpegTaskInterface(this);
    addSubInterface(main, QStringLiteral("mainInterface"), trText("视频压制"));
    addSubInterface(tasks, QStringLiteral("taskInterface"), joinTranslatedLabel(trText("视频压制"), trText("任务")));
    addSubInterface(new FFmpegSettingInterface(this), QStringLiteral("settingInterface"),
                    trText("高级设置"));
    connect(main, &BaseFunctionInterface::taskRequested, tasks,
            [main, tasks](const QString& input, const QString& output) {
                tasks->addSubtitleTask(input, output, main->subtitlePath());
            });
    connect(&GlobalEventBus::instance(), &GlobalEventBus::ffmpeg_requested, tasks,
            &BaseTaskInterface::addTask);
    connect(tasks, &BaseTaskInterface::returnTask, main,
            [main](bool duplicated, const QStringList& paths, bool notify) {
                main->updateTask(duplicated, paths, notify);
            });
}
}
