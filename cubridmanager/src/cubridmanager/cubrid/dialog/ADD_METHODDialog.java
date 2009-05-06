/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution. 
 *
 * Redistribution and use in source and binary forms, with or without modification, 
 * are permitted provided that the following conditions are met: 
 *
 * - Redistributions of source code must retain the above copyright notice, 
 *   this list of conditions and the following disclaimer. 
 *
 * - Redistributions in binary form must reproduce the above copyright notice, 
 *   this list of conditions and the following disclaimer in the documentation 
 *   and/or other materials provided with the distribution. 
 *
 * - Neither the name of the <ORGANIZATION> nor the names of its contributors 
 *   may be used to endorse or promote products derived from this software without 
 *   specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
 * IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, 
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, 
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY 
 * OF SUCH DAMAGE. 
 *
 */

package cubridmanager.cubrid.dialog;

import org.eclipse.swt.widgets.Shell;
import org.eclipse.swt.widgets.Composite;
import org.eclipse.swt.widgets.Display;
import org.eclipse.swt.widgets.Dialog;
import org.eclipse.swt.SWT;

import cubridmanager.ClientSocket;
import cubridmanager.CommonTool;
import cubridmanager.MainRegistry;
import cubridmanager.Messages;
import cubridmanager.cubrid.view.CubridView;
import cubridmanager.cubrid.dialog.PROPPAGE_CLASS_PAGE3Dialog;

import org.eclipse.swt.widgets.Label;
import org.eclipse.swt.widgets.Group;
import org.eclipse.swt.widgets.Button;
import org.eclipse.swt.custom.CLabel;
import org.eclipse.swt.widgets.Text;
import org.eclipse.swt.layout.GridLayout;
import org.eclipse.swt.layout.GridData;
import org.eclipse.swt.layout.FillLayout;

public class ADD_METHODDialog extends Dialog {
	private Shell dlgShell = null;
	private Composite sShell = null;
	private Group group1 = null;
	private Label label2 = null;
	private Button RADIO_ADD_METHOD_INSTANCE = null;
	private Button RADIO_ADD_METHOD_CLASS = null;
	private CLabel clabel1 = null;
	private Label label3 = null;
	private Text EDIT_ADD_METHOD_NAME = null;
	private Label label4 = null;
	private Text EDIT_ADD_METHOD_IMPLEMENTATION = null;
	private Button IDOK = null;
	private Button IDCANCEL = null;
	private boolean ret = false;
	private CLabel cLabel = null;

	public ADD_METHODDialog(Shell parent) {
		super(parent);
	}

	public ADD_METHODDialog(Shell parent, int style) {
		super(parent, style);
	}

	public boolean doModal() {
		createSShell();
		CommonTool.centerShell(dlgShell);
		dlgShell.setDefaultButton(IDOK);
		dlgShell.open();

		Display display = dlgShell.getDisplay();
		while (!dlgShell.isDisposed()) {
			if (!display.readAndDispatch())
				display.sleep();
		}
		return ret;
	}

	private void createSShell() {
		// dlgShell = new Shell(SWT.APPLICATION_MODAL | SWT.DIALOG_TRIM);
		dlgShell = new Shell(getParent(), SWT.APPLICATION_MODAL
				| SWT.DIALOG_TRIM);
		dlgShell.setText(Messages.getString("TITLE.ADD_METHODDIALOG"));
		dlgShell.setLayout(new FillLayout());
		createComposite();
	}

	private void createComposite() {
		GridData gridData7 = new org.eclipse.swt.layout.GridData();
		gridData7.horizontalSpan = 2;
		gridData7.widthHint = 206;
		GridData gridData6 = new org.eclipse.swt.layout.GridData();
		gridData6.horizontalSpan = 2;
		gridData6.widthHint = 206;
		GridData gridData5 = new org.eclipse.swt.layout.GridData();
		gridData5.horizontalSpan = 3;
		gridData5.verticalAlignment = org.eclipse.swt.layout.GridData.CENTER;
		gridData5.heightHint = 3;
		gridData5.horizontalAlignment = org.eclipse.swt.layout.GridData.FILL;
		GridLayout gridLayout1 = new GridLayout();
		gridLayout1.numColumns = 3;
		GridData gridData4 = new org.eclipse.swt.layout.GridData();
		gridData4.widthHint = 88;
		GridData gridData3 = new org.eclipse.swt.layout.GridData();
		gridData3.widthHint = 88;
		GridData gridData2 = new org.eclipse.swt.layout.GridData();
		gridData2.grabExcessHorizontalSpace = true;
		GridData gridData1 = new org.eclipse.swt.layout.GridData();
		gridData1.horizontalSpan = 3;
		gridData1.verticalAlignment = org.eclipse.swt.layout.GridData.FILL;
		gridData1.horizontalAlignment = org.eclipse.swt.layout.GridData.FILL;
		GridLayout gridLayout = new GridLayout();
		gridLayout.numColumns = 3;
		sShell = new Composite(dlgShell, SWT.NONE);
		sShell.setLayout(gridLayout);
		group1 = new Group(sShell, SWT.NONE);
		group1.setLayoutData(gridData1);
		group1.setLayout(gridLayout1);
		label2 = new Label(group1, SWT.LEFT | SWT.WRAP);
		label2.setText(Messages.getString("LABEL.ISCLASSMEMBER"));
		RADIO_ADD_METHOD_INSTANCE = new Button(group1, SWT.RADIO);
		RADIO_ADD_METHOD_INSTANCE.setText(Messages.getString("RADIO.INSTANCE"));
		RADIO_ADD_METHOD_INSTANCE.setSelection(true);
		RADIO_ADD_METHOD_CLASS = new Button(group1, SWT.RADIO);
		RADIO_ADD_METHOD_CLASS.setText(Messages.getString("RADIO.CLASS"));
		RADIO_ADD_METHOD_CLASS.setSelection(false);
		clabel1 = new CLabel(group1, SWT.SHADOW_IN);
		clabel1.setLayoutData(gridData5);
		label3 = new Label(group1, SWT.LEFT | SWT.WRAP);
		label3.setText(Messages.getString("LABEL.METHODNAME"));
		EDIT_ADD_METHOD_NAME = new Text(group1, SWT.BORDER);
		EDIT_ADD_METHOD_NAME.setLayoutData(gridData6);
		label4 = new Label(group1, SWT.LEFT | SWT.WRAP);
		label4.setText(Messages.getString("LABEL.FUNCTIONNAME"));
		EDIT_ADD_METHOD_IMPLEMENTATION = new Text(group1, SWT.BORDER);
		EDIT_ADD_METHOD_IMPLEMENTATION.setLayoutData(gridData7);
		cLabel = new CLabel(sShell, SWT.NONE);
		cLabel.setLayoutData(gridData2);
		IDOK = new Button(sShell, SWT.NONE);
		IDOK.setText(Messages.getString("BUTTON.OK"));
		IDOK.setLayoutData(gridData3);
		IDOK
				.addSelectionListener(new org.eclipse.swt.events.SelectionAdapter() {
					public void widgetSelected(
							org.eclipse.swt.events.SelectionEvent e) {
						String mname = EDIT_ADD_METHOD_NAME.getText().trim();
						if (!MainRegistry.isMultibyteSupport && !CommonTool.isAscii(mname)) {
							CommonTool.ErrorBox(dlgShell, Messages
									.getString("ERROR.INVALIDMETHODNAMENONEASCII"));
							return;
						}
						
						String mimpl = EDIT_ADD_METHOD_IMPLEMENTATION.getText()
								.trim();
						if (mname.length() <= 0 || mimpl.length() <= 0) {
							CommonTool.ErrorBox(dlgShell, Messages
									.getString("ERROR.INPUTMETHODNAME"));
							return;
						}
						String msg = "dbname:" + CubridView.Current_db + "\n";
						msg += "classname:"
								+ PROPPAGE_CLASS_PAGE3Dialog.si.name + "\n";
						msg += "methodname:" + mname + "\n";
						msg += "implementation:" + mimpl + "\n";
						msg += "category:"
								+ ((RADIO_ADD_METHOD_INSTANCE.getSelection()) ? "instance"
										: "class");

						ClientSocket cs = new ClientSocket();

						if (!cs.SendBackGround(dlgShell, msg, "addmethod",
								Messages.getString("WAITING.ADDMETHOD"))) {
							CommonTool.ErrorBox(dlgShell, cs.ErrorMsg);
							return;
						}
						ret = true;
						dlgShell.dispose();
					}
				});
		IDCANCEL = new Button(sShell, SWT.NONE);
		IDCANCEL.setText(Messages.getString("BUTTON.CANCEL"));
		IDCANCEL.setLayoutData(gridData4);
		IDCANCEL
				.addSelectionListener(new org.eclipse.swt.events.SelectionAdapter() {
					public void widgetSelected(
							org.eclipse.swt.events.SelectionEvent e) {
						ret = false;
						dlgShell.dispose();
					}
				});
		dlgShell.pack();
	}

}
