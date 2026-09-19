/**
 * @file qt/call_dialog.cpp Qt UI module -- unified call control dialog
 */
#include "call_dialog.h"

#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPalette>
#include <QIcon>


/* ---- green / red button helpers ---------------------------------- */

static QPushButton *makeButton(const QString &text,
				const QString &iconName,
				bool green)
{
	auto *btn = new QPushButton(text);
	btn->setMinimumSize(96, 44);

	QIcon ic = QIcon::fromTheme(iconName);
	if (!ic.isNull())
		btn->setIcon(ic);

	/* Tint via stylesheet so the colour is visible on any theme. */
	if (green)
		btn->setStyleSheet(
			"QPushButton { background-color: #2e7d32;"
			"              color: white;"
			"              font-weight: bold;"
			"              border-radius: 6px; }"
			"QPushButton:hover { background-color: #388e3c; }"
			"QPushButton:disabled { background-color: #666;"
			"                     color: #aaa; }");
	else
		btn->setStyleSheet(
			"QPushButton { background-color: #c62828;"
			"              color: white;"
			"              font-weight: bold;"
			"              border-radius: 6px; }"
			"QPushButton:hover { background-color: #d32f2f; }"
			"QPushButton:disabled { background-color: #666;"
			"                     color: #aaa; }");

	return btn;
}


/* ---- CallDialog --------------------------------------------------- */

CallDialog::CallDialog(QWidget *parent)
	: QDialog(parent), state_(State::Dialing)
{
	setWindowTitle("Dial");
	setAttribute(Qt::WA_DeleteOnClose, false);
	buildUi();
	applyState();
}


CallDialog::CallDialog(State state, quintptr callPtr,
		       const QString &peerUri, const QString &peerName,
		       QWidget *parent)
	: QDialog(parent), state_(state), callPtr_(callPtr),
	  peerName_(peerName)
{
	setAttribute(Qt::WA_DeleteOnClose, false);
	buildUi();
	uriEdit_->setText(peerUri);
	applyState();
}


void CallDialog::buildUi()
{
	auto *layout = new QVBoxLayout(this);

	auto *label = new QLabel(this);
	layout->addWidget(label);
	label->setText("Enter SIP URI or number:");

	uriEdit_ = new QLineEdit(this);
	uriEdit_->setAlignment(Qt::AlignLeft);
	layout->addWidget(uriEdit_);

	/* In-call DTMF dialpad launcher (hidden unless InCall). */
	dialpadBtn_ = new QPushButton("Dialpad...", this);
	layout->addWidget(dialpadBtn_);
	connect(dialpadBtn_, &QPushButton::clicked,
		this, &CallDialog::onDialpad);

	/* Button row: green on the left, red on the right. */
	auto *btnRow = new QHBoxLayout();
	layout->addLayout(btnRow);

	greenBtn_ = makeButton("Call", "call-start", true);
	redBtn_   = makeButton("Cancel", "call-stop", false);
	btnRow->addWidget(greenBtn_);
	btnRow->addWidget(redBtn_);

	connect(greenBtn_, &QPushButton::clicked,
		this, &CallDialog::onGreen);
	connect(redBtn_, &QPushButton::clicked,
		this, &CallDialog::onRed);

	resize(340, 140);
}


void CallDialog::applyState()
{
	switch (state_) {

	case State::Dialing:
		setWindowTitle("Dial");
		uriEdit_->setReadOnly(false);
		uriEdit_->setPlaceholderText("e.g. +441234567890");
		uriEdit_->clear();
		uriEdit_->setFocus();
		greenBtn_->setText("Call");
		greenBtn_->setEnabled(true);
		redBtn_->setText("Cancel");
		dialpadBtn_->hide();
		break;

	case State::Incoming:
		setWindowTitle(QString("Incoming call: %1").arg(peerName_));
		uriEdit_->setReadOnly(true);
		uriEdit_->setPlaceholderText(QString());
		greenBtn_->setText("Accept");
		greenBtn_->setEnabled(true);
		redBtn_->setText("Reject");
		dialpadBtn_->hide();
		break;

	case State::InCall:
		setWindowTitle(QString("In call: %1").arg(
			uriEdit_->text().isEmpty() ? peerName_ : uriEdit_->text()));
		uriEdit_->setReadOnly(true);
		uriEdit_->setPlaceholderText(QString());
		greenBtn_->setText("Connected");
		greenBtn_->setEnabled(false);
		redBtn_->setText("Hang Up");
		dialpadBtn_->show();
		break;
	}
}


void CallDialog::setStateInCall(const QString &peerUri)
{
	if (!peerUri.isEmpty())
		uriEdit_->setText(peerUri);
	state_ = State::InCall;
	applyState();
}


void CallDialog::setStateIncoming(quintptr callPtr, const QString &peerUri,
				  const QString &peerName)
{
	callPtr_  = callPtr;
	peerName_ = peerName;
	uriEdit_->setText(peerUri);
	state_ = State::Incoming;
	applyState();
	show();
	raise();
	activateWindow();
}


void CallDialog::onGreen()
{
	switch (state_) {

	case State::Dialing: {
		QString uri = uriEdit_->text().trimmed();
		if (uri.isEmpty())
			return;
		emit callRequested(uri);
		/* The caller (TrayApp) will transition us to InCall once
		 * the outgoing call is created. */
		break;
	}

	case State::Incoming:
		emit answerRequested(callPtr_);
		/* TrayApp transitions us to InCall. */
		break;

	case State::InCall:
		/* Nothing -- already connected. */
		break;
	}
}


void CallDialog::onRed()
{
	switch (state_) {

	case State::Dialing:
		/* Just close -- no call was placed. */
		close();
		break;

	case State::Incoming:
		emit rejectRequested(callPtr_);
		break;

	case State::InCall:
		emit hangupRequested(callPtr_);
		break;
	}
}


void CallDialog::onDialpad()
{
	emit dialpadRequested(callPtr_,
		uriEdit_->text().isEmpty() ? peerName_ : uriEdit_->text());
}
