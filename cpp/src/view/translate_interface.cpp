#include "view/translate_interface.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>

#include "common/app_data.h"
#include "common/config.h"
#include "common/config_keys.h"
#include "common/event_bus.h"
#include "common/setting.h"
#include "common/text.h"
#include "common/text_format.h"
#include "service/subtitle_translation.h"
#include "view/translate_task_interface.h"

namespace fkw {
TranslationInterface::TranslationInterface(QWidget* parent)
    : BaseFunctionInterface(trText("翻译"), qfw::FluentIconEnum::Calendar, parent) {
    setObjectName(QStringLiteral("translationInterface"));
    setFileFilter(QStringLiteral("*.srt *.ass"));
    setSpecialFilenameMapping({{QStringLiteral("原文.srt"), QStringLiteral("译文.srt")}});
    connect(&GlobalEventBus::instance(), &GlobalEventBus::translate_requested,
            this, [this](const QString& input, const QString& output) {
        emit taskRequested(input, output);
    });

    // 语言 / AI 模型下拉均绑定 config，config 存代码与模型标识，界面显示本地化文案
    const QList<TranslateOption> languages = translateLanguageOptions();
    QStringList languageLabels;
    QStringList languageValues;
    for (const TranslateOption& option : languages) {
        languageLabels << option.label;
        languageValues << option.value;
    }
    boundChoice(this, settingsGroup_, ConfigKeys::origin_lang,
                qfw::FluentIconEnum::Globe, Text::instance().SourceLanguage,
                Text::instance().SSTL, languageLabels, languageValues);
    boundChoice(this, settingsGroup_, ConfigKeys::target_lang,
                qfw::FluentIconEnum::Language, Text::instance().TargetLanguage,
                Text::instance().SelectTargetLanguage, languageLabels, languageValues);

    const QList<TranslateOption> models = translateAiModelOptions();
    QStringList modelLabels;
    QStringList modelValues;
    for (const TranslateOption& option : models) {
        modelLabels << option.label;
        modelValues << option.value;
    }
    auto* model = boundChoice(this, settingsGroup_, ConfigKeys::ai_model,
                              qfw::FluentIconEnum::BookShelf, Text::instance().AIModel,
                              Text::instance().SelectAIModel, modelLabels, modelValues);

    boundSwitch(this, settingsGroup_, ConfigKeys::useTranslateContext,
                qfw::FluentIconEnum::Unit, Text::instance().EnableContext,
                Text::instance().WEATRSSWDEBTI);

    auto* deepseekModel = boundChoice(
        this, settingsGroup_, ConfigKeys::deepseekModel, qfw::FluentIconEnum::Robot,
        Text::instance().DeepseekModel, Text::instance().SDMV,
        {QStringLiteral("deepseek-v4-flash"), QStringLiteral("deepseek-v4-pro")},
        {QStringLiteral("deepseek-v4-flash"), QStringLiteral("deepseek-v4-pro")});
    auto* reasoning = boundSwitch(this, settingsGroup_, ConfigKeys::deepseekReasoning,
                                  qfw::FluentIconEnum::IOT, Text::instance().DeepThinking,
                                  Text::instance().EDDTM);
    auto updateModel = [model, deepseekModel, reasoning]() {
        const bool show = model->comboBox->currentText() == QStringLiteral("Deepseek");
        deepseekModel->setVisible(show);
        reasoning->setVisible(show);
    };
    connect(model->comboBox, &qfw::ComboBox::currentTextChanged, this, updateModel);
    updateModel();

    auto* card = new qfw::CardWidget(this);
    auto* layout = new QVBoxLayout(card);
    layout->setSpacing(10);
    layout->setContentsMargins(20, 20, 20, 20);
    auto* header = new QHBoxLayout();
    auto* title = new qfw::BodyLabel(
        trText("SRT 内容预览").replace(QStringLiteral("SRT"), QStringLiteral("SRT / ASS")), card);
    title->setStyleSheet(QStringLiteral("font-weight:bold;font-size:14px"));
    statistics_ = new qfw::BodyLabel(trText("共 0 条字幕"), card);
    statistics_->setStyleSheet(QStringLiteral("color:#666"));
    header->addWidget(title);
    header->addStretch();
    header->addWidget(statistics_);
    layout->addLayout(header);
    previewTable_ = new qfw::TableWidget(card);
    previewTable_->setColumnCount(3);
    previewTable_->setHorizontalHeaderLabels({trText("序号"), trText("时间轴"), trText("内容")});
    previewTable_->horizontalHeader()->setStretchLastSection(true);
    previewTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    previewTable_->setAlternatingRowColors(true);
    previewTable_->setMinimumHeight(300);
    layout->addWidget(previewTable_);
    insertPreview(card);
    connect(inputFileCard_->lineEdit, &QLineEdit::textChanged, this,
            &TranslationInterface::loadSubtitlePreview);
}

void TranslationInterface::setInputPath(const QString& path) {
    const bool isAss = subtitleFormatForPath(path) == SubtitleFormat::Ass;
    setOutputSuffix(isAss ? QStringLiteral("_translated.ass") : QStringLiteral("_translated.srt"));
    BaseFunctionInterface::setInputPath(path);
    const QFileInfo input(path);
    if (isAss && input.completeBaseName() == QStringLiteral("原文"))
        outputFileCard_->lineEdit->setText(input.dir().filePath(QStringLiteral("译文.ass")));
}

bool TranslationInterface::validateBeforeStart(QString* errorMessage) {
    // 对齐 Python TranslationInterface._start_processing 的前置校验
    const auto& cfg = AppConfig::instance();
    const auto& t = Text::instance();
    const auto fail = [errorMessage](const QString& message) {
        if (errorMessage) *errorMessage = message;
        return false;
    };
    const QString ai = cfg.value(ConfigKeys::ai_model).toString();
    if (ai == QStringLiteral("deepseek") &&
        cfg.value(ConfigKeys::deepseekApiKey).toString().isEmpty())
        return fail(t.PFIYDAKF);
    if (ai == QStringLiteral("glm-4.5-flash") &&
        cfg.value(ConfigKeys::glmApiKey).toString().isEmpty())
        return fail(t.PFIYG45FAKF);
    if (ai == QStringLiteral("spark-lite") &&
        cfg.value(ConfigKeys::sparkApiKey).toString().isEmpty())
        return fail(t.PFIYSLAKF);
    if (ai == QStringLiteral("hunyuan-turbos-latest") &&
        cfg.value(ConfigKeys::hunyuanApiKey).toString().isEmpty())
        return fail(t.PFIYTHAKF);
    if (ai == QStringLiteral("intern-latest") &&
        cfg.value(ConfigKeys::internApiKey).toString().isEmpty())
        return fail(t.PFIYIAKF);
    if (ai == QStringLiteral("ernie-speed-128k") &&
        cfg.value(ConfigKeys::ernieSpeedApiKey).toString().isEmpty())
        return fail(t.PFIYBES1AKF);
    if (ai == QStringLiteral("gemini-3.5-flash") &&
        cfg.value(ConfigKeys::geminiApiKey).toString().isEmpty())
        return fail(t.PFIYG3FAKF);
    if (ai == QStringLiteral("custom-model")) {
        if (!cfg.value(ConfigKeys::customModelEnabled).toBool()) return fail(t.PECMISF);
        if (cfg.value(ConfigKeys::customModelApiKey).toString().isEmpty())
            return fail(t.PFIYCMAKF);
        if (cfg.value(ConfigKeys::customModelBaseUrl).toString().isEmpty())
            return fail(t.PFIYCMABUF);
        if (cfg.value(ConfigKeys::customModelName).toString().isEmpty())
            return fail(t.PFIYCMNF);
    }
    if (cfg.value(ConfigKeys::origin_lang).toString() ==
        cfg.value(ConfigKeys::target_lang).toString())
        return fail(t.SATLATS);
    return true;
}

void TranslationInterface::loadSubtitlePreview(const QString& path) {
    previewTable_->setRowCount(0);
    QString contents, error;
    qsizetype count = 0;
    const auto addRow = [this](int row, const QString& index, const QString& time, const QString& text) {
        previewTable_->insertRow(row);
        previewTable_->setItem(row, 0, new QTableWidgetItem(index));
        previewTable_->setItem(row, 1, new QTableWidgetItem(time));
        previewTable_->setItem(row, 2, new QTableWidgetItem(text));
    };
    constexpr qsizetype previewLimit = 500;
    if (readSubtitleForTranslation(path, contents, error)) {
        if (subtitleFormatForPath(path) == SubtitleFormat::Ass) {
            const auto cues = parseAss(contents);
            count = cues.size();
            for (int i = 0; i < qMin(count, previewLimit); ++i) {
                const auto& cue = cues.at(i);
                addRow(i, QString::number(i + 1), cue.time, cue.previewText);
            }
        } else {
            const auto items = parseSrt(contents);
            count = items.size();
            for (int i = 0; i < qMin(count, previewLimit); ++i) {
                const auto& item = items.at(i);
                QString text = item.text;
                addRow(i, item.index, item.time, text.replace(u'\n', QStringLiteral("\\n")));
            }
        }
    }
    QString total = Text::instance().Text0SubtitlesTotal;
    statistics_->setText(total.replace(QLatin1Char('0'), QString::number(count)));
}

TranslateStackedInterfaces::TranslateStackedInterfaces(QWidget* parent)
    : BaseStackedInterfaces(parent) {
    setObjectName(QStringLiteral("TranslateStackedInterfaces"));
    auto* main = new TranslationInterface(this);
    auto* tasks = new TranslateTaskInterface(this);
    addSubInterface(main, QStringLiteral("mainInterface"), trText("翻译字幕"));
    addSubInterface(tasks, QStringLiteral("taskInterface"), joinTranslatedLabel(trText("翻译字幕"), trText("任务")));
    addSubInterface(new TranslateSettingInterface(this), QStringLiteral("settingInterface"),
                    trText("高级设置"));
    connect(main, &BaseFunctionInterface::taskRequested, tasks,
            [tasks](const QString& input, const QString& output) {
                tasks->addTask(input, output);
            });
    connect(tasks, &BaseTaskInterface::returnTask, main,
            [main](bool duplicated, const QStringList& paths, bool notify) {
                main->updateTask(duplicated, paths, notify);
            });
}
}
