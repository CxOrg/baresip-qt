/**
 * @file qt/dialpad_dialog.h Qt UI module -- DTMF dialpad
 */
#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;

class DialpadDialog : public QDialog {
	Q_OBJECT

public:
	/** callPtr is the opaque struct call* this dialpad sends digits
	 *  for. peerLabel is shown in the title bar for context.
	 */
	DialpadDialog(quintptr callPtr, const QString &peerLabel,
		      QWidget *parent = nullptr);

private slots:
	void pressDigit(char key);

private:
	quintptr callPtr_;
	QLineEdit *log_ = nullptr;
};
