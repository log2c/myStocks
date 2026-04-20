# 一、实时行情接口（最常用）

## 1. 新浪实时行情

接口：

```
https://hq.sinajs.cn/list=sh600519
```

批量：

```
https://hq.sinajs.cn/list=sh600519,sz000001
```

特点：

* 秒级刷新
* 返回 JS 字符串
* 字段最多（30+）

使用最广。

---

## 2. 腾讯实时行情

接口：

```
https://qt.gtimg.cn/q=sh600519
```

批量：

```
https://qt.gtimg.cn/q=sh600519,sz000001
```

特点：

* 返回 `~` 分隔
* 简单稳定
* 速度快

很多 GitHub 项目默认源。

---

## 3. 网易财经实时行情

接口：

```
http://api.money.126.net/data/feed/0600519
```

多个股票：

```
http://api.money.126.net/data/feed/0600519,1000001
```

说明：

* 上交所：0+股票代码
* 深交所：1+股票代码

例如：

```
0600519  贵州茅台
1000001  平安银行
```

返回：

JSON。

---

# 二、K线 / 历史数据接口

## 1. 腾讯 K线接口

日线：

```
https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=sh600519,day,,,1000,qfq
```

参数：

```
股票,周期,开始,结束,数量,复权
```

周期：

```
day
week
month
```

返回：

JSON。

---

## 2. 新浪 K线

接口：

```
https://finance.sina.com.cn/realstock/company/sh600519/hisdata/klc_kl.js
```

还有：

```
minute
day
week
month
```

---

## 3. 东方财富 K线（最强）

接口：

```
https://push2his.eastmoney.com/api/qt/stock/kline/get
```

示例：

```
https://push2his.eastmoney.com/api/qt/stock/kline/get?secid=1.600519&klt=101&fqt=1
```

参数：

| 参数    | 含义    |
| ----- | ----- |
| secid | 市场+股票 |
| klt   | 周期    |
| fqt   | 复权    |

周期：

```
1   1分钟
5   5分钟
15  15分钟
30  30分钟
60  60分钟
101 日线
102 周线
103 月线
```

这个接口 **AkShare / TuShare 都大量使用**。

---

# 三、资金流接口

## 东方财富资金流

接口：

```
https://push2.eastmoney.com/api/qt/stock/get
```

示例：

```
https://push2.eastmoney.com/api/qt/stock/get?secid=1.600519&fields=f62,f66,f72,f78
```

字段：

| 字段  | 含义    |
| --- | ----- |
| f62 | 主力净流入 |
| f66 | 超大单   |
| f72 | 大单    |
| f78 | 中单    |

---

# 四、板块接口

## 东方财富板块

接口：

```
https://push2.eastmoney.com/api/qt/clist/get
```

示例：

```
https://push2.eastmoney.com/api/qt/clist/get?pn=1&pz=50&fs=m:90+t:2
```

返回：

* 行业板块
* 概念板块
* 成分股

AkShare 大量使用。

---

# 五、分时数据接口

## 东方财富分时

接口：

```
https://push2.eastmoney.com/api/qt/stock/trends2/get
```

示例：

```
https://push2.eastmoney.com/api/qt/stock/trends2/get?secid=1.600519
```

返回：

```
分时价格
成交量
均价
```

---

# 六、指数接口

## 腾讯指数

```
https://qt.gtimg.cn/q=sh000001
```

常见指数：

| 指数   | 代码       |
| ---- | -------- |
| 上证指数 | sh000001 |
| 深证成指 | sz399001 |
| 创业板  | sz399006 |

---

# 七、雪球接口

雪球 API：

```
https://stock.xueqiu.com/v5/stock/quote.json
```

示例：

```
https://stock.xueqiu.com/v5/stock/quote.json?symbol=SH600519
```

注意：

需要 cookie。

---

# 八、同花顺接口

同花顺接口较复杂：

```
http://d.10jqka.com.cn/v6/time/33_600519/last.js
```

特点：

* JS 格式
* 参数带校验

很多项目用它做分时。

---

# 九、Level2 / Tick接口（非免费）

常见：

* Wind
* 通联
* 聚宽
* 米筐
* 迅投

这些一般：

```
tick
逐笔成交
盘口10档
```

延迟：

```
< 100ms
```

但基本收费。

---

# 十、最常见数据源总结

| 数据源  | 实时 | K线 | 资金流 |
| ---- | -- | -- | --- |
| 新浪   | ✔  | ✔  | ✖   |
| 腾讯   | ✔  | ✔  | ✖   |
| 网易   | ✔  | ✔  | ✖   |
| 东方财富 | ✔  | ✔  | ✔   |
| 雪球   | ✔  | ✔  | ✔   |
| 同花顺  | ✔  | ✔  | ✔   |

---

# 十一、量化圈常用架构

真实项目一般这样用：

```
实时行情   → 腾讯
K线数据   → 东方财富
资金流     → 东方财富
备用数据   → 新浪
```

原因：

* 腾讯稳定
* 东方财富数据最全

---

💡 如果你愿意，我可以再给你一份 **量化交易最完整接口地图（30+ A股隐藏API）**，包括：

* 龙虎榜接口
* 北向资金接口
* 融资融券接口
* 涨停板接口
* 打板数据接口
* 情绪周期数据接口

很多 **游资/量化团队其实就是用这些接口做监控系统**。
