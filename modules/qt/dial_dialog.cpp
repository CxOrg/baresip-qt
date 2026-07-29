/**
 * @file qt/dial_dialog.cpp Qt UI module -- dial dialog
 */
#include "dial_dialog.h"

#include <QLineEdit>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>


DialDialog::DialDialog(QWidget *parent)
	: QDialog(parent)
{
	setWindowTitle("Dial");
	setModal(true);

	auto *layout = new QVBoxLayout(this);

	layout->addWidget(new QLabel("Enter SIP URI or number:"));

	uriEdit_ = new QLineEdit(this);
	layout->addWidget(uriEdit_);

	auto *buttons = new QDialogButtonBox(
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	buttons->button(QDialogButtonBox::Ok)->setText("Call");
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);

	resize(320, 100);
}


bool DialDialog::getUri(QString &uri)
{
	uriEdit_->clear();
	uriEdit_->setFocus();

	if (exec() == QDialog::Accepted) {
		uri = uriEdit_->text().trimmed();
		return !uri.isEmpty();
	}

	return false;
}
