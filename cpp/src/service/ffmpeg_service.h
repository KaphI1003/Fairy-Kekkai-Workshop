#pragma once

#include "service/subtitle_encode.h"
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <memory>

#include "common/logger.h"
#include "service/task_base.h"

namespace fkw {

// 对应 app/service/ffmpeg_service.py 的 FFmpegTask：纯数据类，字段对齐
// Easy-FFmpeg。公共字段（taskId/fileName/outputName/logPath/createTime）在
// TaskBase 中，这里只补充压制专有参数。Python 中任务对象被界面/卡片/Worker
// 共享引用，这里用 shared_ptr 表达同一语义。
struct FFmpegTask : TaskBase {
    FFmpegTask(const QString& videoPath, const QString& outputPath, const QString& subtitle = {});

    QString saveFolder;
    QString subtitlePath;
    bool preferGpu;
};

// 根据配置修正输出文件扩展名（纯函数，无 I/O）。
QString adjustOutputFormat(const QString& outputFile);

// FFmpeg 压制任务在工作线程内探测片源、合成字幕并提交成片。
// 由 QThreadPool 调度并在 run() 结束后 deleteLater() 回收，
// 外部若需持有引用请使用 QPointer。
class FFmpegWorker : public TaskWorker {
    Q_OBJECT
public:
    explicit FFmpegWorker(std::shared_ptr<FFmpegTask> task);

    void run() override;

    // 取消任务：标记并 kill 当前进程。
    // 取消的任务不 emit finishTaskSig（状态已由任务界面设为 Cancelled）。
    void cancel() override;

private:
    bool runStage(const QStringList& args, bool* nvencFailed = nullptr);
    void parseProgress(const QString& data);
    bool encode(QString& error);
    void finish(bool success);

    std::shared_ptr<FFmpegTask> task_;
    double duration_ = 0.0;
    double lastEmit_ = 0.0;
    QSharedPointer<Logger> taskLogger_;

};

}  // namespace fkw
