#pragma once
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include "components/base_function_interface.h"
#include "components/base_stacked_interface.h"
#include "components/base_task_interface.h"
#include "service/interpolation_service.h"
namespace fkw {
class PreviewCanvas;
class VideoFrameService;
class InterpolationInterface : public BaseFunctionInterface {
    Q_OBJECT
public:
    explicit InterpolationInterface(QWidget* parent = nullptr);
    ~InterpolationInterface() override;
    InterpolationOptions options() const;
    double previewStartSeconds() const;
signals:
    void previewRequested(InterpolationWorker* worker);
    void cancelPreviewRequested(InterpolationWorker* worker);
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
    QString probedPath_, generatedOutput_, previewPath_, previewSource_, sourceVideoPath_;
    double previewSourceStart_ = 0;
    double previewSourceFps_ = 0;
    InterpolationOptions previewOptions_;
    QProcess* probe_;
    QTimer probeDelay_, seekDelay_;
    VideoFrameService* sourceFrames_;
    PreviewCanvas* sourceCanvas_;
    qfw::Slider* sourceSlider_;
    qfw::CaptionLabel* sourceFrameLabel_;
    qfw::CaptionLabel* sourceTimeLabel_;
    int displayedSourceFrame_ = -1;
    qfw::BodyLabel *sourceInfo_, *targetInfo_;
    qfw::ComboBox *rateMode_, *preset_, *multiplier_, *model_, *quality_, *codec_, *audio_, *container_;
    qfw::SettingCard *presetCard_, *multiplierCard_;
    QWidget* customCard_;
    qfw::SpinBox* customRate_;
    qfw::SwitchSettingCard *reuseHolds_, *protectCuts_;
    qfw::PushButton *previewButton_, *cancelPreview_, *viewPreview_;
    qfw::ProgressBar* previewProgress_;
    qfw::BodyLabel* previewStatus_;
    qfw::CaptionLabel* previewStats_;
    QPointer<InterpolationWorker> previewWorker_;
};
class InterpolationTaskInterface : public BaseTaskInterface {
    Q_OBJECT
public:
    explicit InterpolationTaskInterface(QWidget* parent = nullptr);
    void enqueue(const QString& input, const QString& output, const InterpolationOptions& options);
    void enqueuePreview(InterpolationWorker* worker);
    void cancelPreview(InterpolationWorker* worker);
protected:
    std::shared_ptr<TaskBase> createTask(const QString& input, const QString& output) override;
    TaskWorker* createWorker(const std::shared_ptr<TaskBase>& task) override;
    QString getTaskPath(const std::shared_ptr<TaskBase>& task) const override;
    QString taskTypeText() const override;
private:
    InterpolationOptions pendingOptions_;
};
class InterpolationSettingInterface : public AdvancedSettingInterface {
    Q_OBJECT
public:
    explicit InterpolationSettingInterface(QWidget* parent = nullptr);
};
class InterpolationStackedInterfaces : public BaseStackedInterfaces {
    Q_OBJECT
public:
    explicit InterpolationStackedInterfaces(QWidget* parent = nullptr);
};
}
