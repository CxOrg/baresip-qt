/**
 * @file qt/dial_dialog.h Qt UI module -- dial dialog
 */
#pragma once

#include <QDialog>

class QLineEdit;

class DialDialog : public QDialog {
	Q_OBJECT

public:
	explicit DialDialog(QWidget *parent = nullptr);

	/** Shows the dialog modally. Returns true and fills `uri` if the
	 *  user confirmed, false if they cancelled.
	 */
	bool getUri(QString &uri);

private:
	QLineEdit *uriEdit_ = nullptr;
};
