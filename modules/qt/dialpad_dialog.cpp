/**
 * @file qt/dialpad_dialog.cpp Qt UI module -- DTMF dialpad
 */
#include "dialpad_dialog.h"
#include "qt_mod.h"

#include <QGridLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLineEdit>


DialpadDialog::DialpadDialog(quintptr callPtr, const QString &peerLabel,
			      QWidget *parent)
	: QDialog(parent), callPtr_(callPtr)
{
	setWindowTitle(QString("Dialpad: %1").arg(peerLabel));
	setWindowFlag(Qt::Tool, true);
	setAttribute(Qt::WA_DeleteOnClose, true);

	auto *layout = new QVBoxLayout(this);

	log_ = new QLineEdit(this);
	log_->setReadOnly(true);
	log_->setAlignment(Qt::AlignRight);
	log_->setPlaceholderText("Keys sent this session");
	layout->addWidget(log_);

	auto *grid = new QGridLayout();
	layout->addLayout(grid);

	static const char *keys[4][3] = {
		{ "1", "2", "3" },
		{ "4", "5", "6" },
		{ "7", "8", "9" },
		{ "*", "0", "#" },
	};

	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 3; col++) {
			const char *label = keys[row][col];
			auto *btn = new QPushButton(label, this);
			btn->setMinimumSize(48, 48);
			char key = label[0];
			connect(btn, &QPushButton::clicked,
				this, [this, key]() { pressDigit(key); });
			grid->addWidget(btn, row, col);
		}
	}

	resize(220, 260);
}


void DialpadDialog::pressDigit(char key)
{
	qt_mod_send_digit(reinterpret_cast<struct call *>(callPtr_), key);
	log_->setText(log_->text() + QChar(key));
}
