// pages/index/index.js

function formatTime(timestamp) {
  const ts = Number(timestamp);
  if (isNaN(ts) || ts <= 0) return '暂无数据';
  let msTs = ts < 1000000000000 ? ts * 1000 : ts;
  const date = new Date(msTs);
  if (isNaN(date.getTime())) return '暂无数据';
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, '0');
  const day = String(date.getDate()).padStart(2, '0');
  const hour = String(date.getHours()).padStart(2, '0');
  const minute = String(date.getMinutes()).padStart(2, '0');
  const second = String(date.getSeconds()).padStart(2, '0');
  return `${year}-${month}-${day} ${hour}:${minute}:${second}`;
}

Page({
  data: {
    navPaddingTop: '120rpx',
    temperature: '--',
    humidity: '--',
    lastUpdate: '暂无数据',
    statusClass: 'offline',
    statusText: '设备离线',
    isRefreshing: false,
    deviceOnline: false,
    isSwitching: false,
    lastGlobalSwitchTime: 0,
    isSendingIR: false,          // 红外按钮防重复状态

    // 统一管理所有开关的显示状态（保留 Ir_launch 用于数据同步，但不展示开关）
    switches: {
      Led_switch: false,
      Fan_switch: false,
      Hum_switch: false,
      Servo_switch: false,
      Music_switch: false,
      Ir_launch: false
    },

    localOperate: {}
  },

  onLoad(options) {
    this.calcNavPadding();
    this.getDeviceData();
    this.startAutoRefresh(30000);
  },

  onUnload() {
    this.clearAllTimers();
  },

  startAutoRefresh(interval = 30000) {
    this.clearAllTimers();
    this.dataTimer = setInterval(() => {
      this.getDeviceData();
    }, interval);
  },

  clearAllTimers() {
    if (this.dataTimer) { clearInterval(this.dataTimer); this.dataTimer = null; }
    if (this.delayTimer) { clearTimeout(this.delayTimer); this.delayTimer = null; }
  },

  calcNavPadding() {
    try {
      const systemInfo = wx.getSystemInfoSync();
      const menuButton = wx.getMenuButtonBoundingClientRect();
      const navTop = systemInfo.statusBarHeight + (menuButton.top - systemInfo.statusBarHeight) * 2;
      this.setData({ navPaddingTop: `${(navTop * 750 / systemInfo.screenWidth) + 20}rpx` });
    } catch (error) {
      this.setData({ navPaddingTop: '120rpx' });
    }
  },

  getDeviceData() {
    if (this.data.isRefreshing || this.data.isSwitching) return;

    this.setData({ statusText: '获取数据中...', isRefreshing: true });
    const now = Date.now();

    wx.request({
      url: 'https://iot-api.heclouds.com/thingmodel/query-device-property',
      method: 'GET',
      header: {
        'authorization': 'version=2018-10-31&res=products%2FAfw3tvB0Br%2Fdevices%2FESP32_02&et=1924763256&method=md5&sign=jEGskX%2Bh9irK1cytq%2FS8UQ%3D%3D',
        'Accept': 'application/json, text/plain, */*'
      },
      data: {
        product_id: 'Afw3tvB0Br',
        device_name: 'ESP32_02'
      },
      success: (res) => {
        let tempVal = '--';
        let humiVal = '--';
        let newSwitches = { ...this.data.switches };
        let updatedLocalOperate = { ...this.data.localOperate };
        let latestDataTime = 0;
        let isOnline = false;

        if (res.data.code === 0 && res.data.data) {
          res.data.data.forEach(item => {
            if (item.identifier === 'Temperature') tempVal = item.value;
            if (item.identifier === 'Humidity') humiVal = item.value;

            // 统一处理所有开关（包含 Ir_launch）
            if (['Led_switch', 'Fan_switch', 'Hum_switch', 'Servo_switch', 'Music_switch', 'Ir_launch'].includes(item.identifier)) {
              const key = item.identifier;
              const cloudState = (typeof item.value === 'string') ? (item.value.toLowerCase() === 'true') : Boolean(item.value);

              const operateRecord = updatedLocalOperate[key];
              const isInProtectionWindow = operateRecord && (now - operateRecord.time) < 15000;

              if (isInProtectionWindow) {
                newSwitches[key] = operateRecord.target;
              } else {
                newSwitches[key] = cloudState;
                if (operateRecord) delete updatedLocalOperate[key];
              }
            }

            if (item.time && Number(item.time) > 0) latestDataTime = Number(item.time);
          });

          if (latestDataTime > 0) {
            const dataAge = now - (latestDataTime.toString().length === 10 ? latestDataTime * 1000 : latestDataTime);
            isOnline = dataAge < 40 * 1000;
          }
        }

        this.setData({
          temperature: isOnline ? `${tempVal} °C` : '--',
          humidity: isOnline ? `${humiVal} %` : '--',
          switches: newSwitches,
          localOperate: updatedLocalOperate,
          lastUpdate: formatTime(latestDataTime),
          deviceOnline: isOnline,
          statusText: isOnline ? '设备在线' : '设备离线',
          statusClass: isOnline ? 'online' : 'offline',
          isRefreshing: false
        });
      },
      fail: (err) => {
        this.setData({
          statusText: '网络请求失败',
          isRefreshing: false,
          temperature: '--',
          humidity: '--',
          deviceOnline: false
        });
        wx.showToast({ title: '网络请求失败', icon: 'none' });
      }
    });
  },

  onRefreshTap() {
    this.getDeviceData();
  },

  // 原有设备开关处理（Led, Fan, Hum, Servo, Music）
  onDeviceSwitchChange(e) {
    const key = e.currentTarget.dataset.key;
    const newState = e.detail.value;
    const now = Date.now();

    if (now - this.data.lastGlobalSwitchTime < 1000) {
      this.setData({ [`switches.${key}`]: !newState });
      return;
    }
    this.setData({ lastGlobalSwitchTime: now });

    if (this.data.isSwitching || !this.data.deviceOnline) {
      this.setData({ [`switches.${key}`]: !newState });
      if (!this.data.deviceOnline) wx.showToast({ title: '设备离线', icon: 'none' });
      return;
    }

    const newLocalOperate = { ...this.data.localOperate };
    newLocalOperate[key] = { target: newState, time: now };

    this.setData({
      localOperate: newLocalOperate,
      [`switches.${key}`]: newState,
      isSwitching: true
    });

    this.clearAllTimers();

    let params = {};
    params[key] = newState;

    wx.request({
      url: 'https://iot-api.heclouds.com/thingmodel/set-device-property',
      method: 'POST',
      header: {
        'authorization': 'version=2018-10-31&res=products%2FAfw3tvB0Br%2Fdevices%2FESP32_02&et=1924763256&method=md5&sign=jEGskX%2Bh9irK1cytq%2FS8UQ%3D%3D',
        'Content-Type': 'application/json'
      },
      data: {
        product_id: 'Afw3tvB0Br',
        device_name: 'ESP32_02',
        params: params
      },
      success: (res) => {
        if (res.data.code === 0) {
          wx.showToast({ title: '指令已发送', icon: 'success' });
          this.delayTimer = setTimeout(() => {
            const currentLocal = { ...this.data.localOperate };
            delete currentLocal[key];
            this.setData({ localOperate: currentLocal });
            this.startAutoRefresh(30000);
            this.getDeviceData();
          }, 30000);
        } else {
          this.handleControlFail(key, newState, res.data.msg);
        }
      },
      fail: () => this.handleControlFail(key, newState, '网络请求失败'),
      complete: () => this.setData({ isSwitching: false })
    });
  },

  handleControlFail(key, newState, msg) {
    wx.showToast({ title: `控制失败: ${msg}`, icon: 'none' });
    const currentLocal = { ...this.data.localOperate };
    delete currentLocal[key];
    this.setData({
      [`switches.${key}`]: !newState,
      localOperate: currentLocal
    });
    this.startAutoRefresh(30000);
  },

  // ==================== 红外发射按钮（新增） ====================
  sendIRCommand() {
    if (this.data.isSendingIR) return;
    if (!this.data.deviceOnline) {
      wx.showToast({ title: '设备离线', icon: 'none' });
      return;
    }

    this.setData({ isSendingIR: true });

    wx.request({
      url: 'https://iot-api.heclouds.com/thingmodel/set-device-property',
      method: 'POST',
      header: {
        'authorization': 'version=2018-10-31&res=products%2FAfw3tvB0Br%2Fdevices%2FESP32_02&et=1924763256&method=md5&sign=jEGskX%2Bh9irK1cytq%2FS8UQ%3D%3D',
        'Content-Type': 'application/json'
      },
      data: {
        product_id: 'Afw3tvB0Br',
        device_name: 'ESP32_02',
        params: {
          Ir_launch: true
        }
      },
      success: (res) => {
        if (res.data.code === 0) {
          wx.showToast({ title: '红外信号已发射', icon: 'success' });
        } else {
          wx.showToast({ title: '发射失败', icon: 'none' });
        }
      },
      fail: () => {
        wx.showToast({ title: '网络请求失败', icon: 'none' });
      },
      complete: () => {
        this.setData({ isSendingIR: false });
      }
    });
  }
});