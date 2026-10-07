import QtQuick

QtObject {
    id: model

    property int currentStep: 0
    property string username: mirage.steamUsername
    property string password: ""
    property string guardCode: ""
    property bool passwordVisible: false
    property bool showLog: false
    property bool busy: loginBusy
    property bool loginBusy: loginState === "loggingIn"
    property string loginState: mirage.steamLoginState
    property string loginMessage: mirage.steamLoginMessage
    property string guardType: mirage.steamGuardType
    property string qrChallengeUrl: mirage.steamQRCodeUrl
    property bool hasSavedSession: mirage.steamSessionReusable
        || (mirage.steamLoggedIn && username.length > 0)
    property bool sessionReusable: hasSavedSession
    property bool canProceed: {
        if (currentStep === 0 || currentStep === 2)
            return true;
        return loginState === "success";
    }

    function refreshFromService() {
        // 状态属性均为绑定，进入窗口时无需手动刷新。
    }

    function loginWithQR() {
        mirage.loginSteamQR();
    }

    function nextStep() {
        if (currentStep < 2 && canProceed)
            currentStep += 1;
    }

    function previousStep() {
        if (currentStep <= 0)
            return;
        if (currentStep === 1)
            mirage.cancelSteamLogin();
        currentStep -= 1;
    }

    function cancelPendingWork() {
        mirage.cancelPendingSteamWork();
    }

    function useSavedSession() {
        mirage.useSavedSteamSession();
    }

    function completeSetup() {
        // 登录已完成（loginState === "success"）；关闭由视图层处理。
    }
}
