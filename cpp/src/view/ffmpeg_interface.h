#pragma once
#include "components/base_function_interface.h"
#include "components/base_stacked_interface.h"
namespace fkw {
class FFmpegInterface : public BaseFunctionInterface {
    Q_OBJECT
public:
    explicit FFmpegInterface(QWidget* parent = nullptr);
    void setInputPath(const QString& path) override;
    QString subtitlePath() const;
protected:
    bool validateBeforeStart(QString* error) override;
private:
    ChooseFileSettingCard* subtitles_;
};
class FFmpegStackedInterfaces : public BaseStackedInterfaces {
    Q_OBJECT
public:
    explicit FFmpegStackedInterfaces(QWidget* parent = nullptr);
};
}
