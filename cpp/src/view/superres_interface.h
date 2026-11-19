#pragma once

#include <QPointer>
#include <QProcess>
#include <QTimer>
#include "components/base_function_interface.h"
#include "components/base_stacked_interface.h"
#include "components/base_task_interface.h"
#include "service/superres_service.h"

namespace fkw {
class PreviewCanvas;
class VideoFrameService;
class SuperresInterface : public BaseFunctionInterface {
    Q_OBJECT
public:
    explicit SuperresInterface(QWidget* parent = nullptr);
    ~SuperresInterface() override;
    SuperresOptions options() const;
    double previewStartSeconds() const;
signals:
    void previewRequested(SuperresWorker* worker);
    void cancelPreviewRequested(SuperresWorker* worker);
protected:
    bool validateBeforeStart(QString* error) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
private:
    void probeVideo();
    void refreshTarget();
    void startPreview();
    void showPreview();
    void updatePreviewAvailability();
    void updateSourcePosition();
    SuperresVideoInfo videoInfo_;
    QString probedPath_;
    QString generatedOutput_;
    QString previewPath_;
    QString previewSource_;
    double previewSourceStart_ = 0;
    SuperresOptions previewOptions_;
    QProcess* probe_;
    QTimer probeDelay_;
    QTimer seekDelay_;
    VideoFrameService* sourceFrames_;
    PreviewCanvas* sourceCanvas_;
    qfw::Slider* sourceSlider_;
    qfw::CaptionLabel* sourceFrameLabel_;
    qfw::CaptionLabel* sourceTimeLabel_;
    QString sourceVideoPath_;
    int displayedSourceFrame_ = -1;
    qfw::BodyLabel* sourceInfo_;
    qfw::BodyLabel* targetInfo_;
    qfw::ComboBox* sizeMode_;
    qfw::ComboBox* preset_;
    qfw::ComboBox* scale_;
    qfw::ComboBox* model_;
    qfw::SwitchSettingCard* reuseFrames_;
    qfw::ComboBox* quality_;
    qfw::ComboBox* codec_;
    qfw::ComboBox* audio_;
    qfw::ComboBox* container_;
    qfw::ComboBox* fit_;
    qfw::SpinBox* width_;
    qfw::SpinBox* height_;
    qfw::CheckBox* lockRatio_;
    QWidget* customRow_;
    qfw::SettingCard* presetCard_;
    qfw::SettingCard* scaleCard_;
    qfw::PushButton* previewButton_;
    qfw::PushButton* cancelPreview_;
    qfw::PushButton* viewPreview_;
    qfw::ProgressBar* previewProgress_;
    qfw::BodyLabel* previewStatus_;
    qfw::CaptionLabel* previewReuseInfo_;
    QPointer<SuperresWorker> previewWorker_;
};

class SuperresTaskInterface : public BaseTaskInterface {
    Q_OBJECT
public:
    explicit SuperresTaskInterface(QWidget* parent = nullptr);
    void enqueue(const QString& input, const QString& output, const SuperresOptions& options);
    void enqueuePreview(SuperresWorker* worker);
    void cancelPreview(SuperresWorker* worker);
protected:
    std::shared_ptr<TaskBase> createTask(const QString& input, const QString& output) override;
    TaskWorker* createWorker(const std::shared_ptr<TaskBase>& task) override;
    QString getTaskPath(const std::shared_ptr<TaskBase>& task) const override;
    QString taskTypeText() const override;
private:
    SuperresOptions pendingOptions_;
};

class SuperresSettingInterface : public AdvancedSettingInterface {
    Q_OBJECT
public:
    explicit SuperresSettingInterface(QWidget* parent = nullptr);
};

class SuperresStackedInterfaces : public BaseStackedInterfaces {
    Q_OBJECT
public:
    explicit SuperresStackedInterfaces(QWidget* parent = nullptr);
};
} // namespace fkw
