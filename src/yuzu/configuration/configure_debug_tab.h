// Copyright 2021 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <QWidget>

namespace Ui {
class ConfigureDebugTab;
}

class ConfigureDebugTab : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureDebugTab(QWidget* parent = nullptr);
    ~ConfigureDebugTab() override;

    void ApplyConfiguration();

    void SetCurrentIndex(int index);

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    void SetConfiguration();

    std::unique_ptr<Ui::ConfigureDebugTab> ui;
};
