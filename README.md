# Canoroid

ถ่ายภาพสไตล์กล้อง instant บน Canon EOS 600D ด้วยฐาน Magic Lantern รุ่นทดลองนี้นำ JPEG จากการ์ดมาครอปตรงกลางเป็น 1:1 เลือกกรอบได้สามแบบ และบันทึกภาพสีขนาด 1080 × 1080 เพิ่มอีกไฟล์

## Canoroid v0.1.0-alpha (รุ่นที่ 11)

สำหรับ **Canon EOS 600D เฟิร์มแวร์ Canon 1.0.2** เลือกกรอบแล้วกด SET เพื่อเข้า Live View ถ่าย JPEG หรือ RAW+JPEG ทีละภาพ แล้วเปรียบเทียบต้นฉบับกับภาพใส่กรอบได้ในหน้าผลลัพธ์ รุ่นที่ 11 ปรับการส่งภาพขึ้นจอหลัง Quick Review เพิ่มปุ่มในหน้าดูภาพและตัวอย่างก่อนยืนยันลบ พร้อมเว้นแนวกรอบเล็งบริเวณแถบค่าถ่ายภาพ รุ่นนี้ผ่านการบิลด์ ARM และทดสอบบนคอมพิวเตอร์แล้ว ยังรอตรวจจอบน 600D จริง

- ติดตั้งครั้งแรก มีตัวติดตั้ง ML-SETUP.FIR — [ดาวน์โหลด FIRST-INSTALL](https://raw.githubusercontent.com/pongallery/Canoroid/v0.1.0-alpha.11/downloads/Canoroid-v0.1.0-alpha.11-FIRST-INSTALL.zip)
- อัปเดตกล้องและการ์ดที่บูต Magic Lantern ได้อยู่แล้ว — [ดาวน์โหลด UPDATE](https://raw.githubusercontent.com/pongallery/Canoroid/v0.1.0-alpha.11/downloads/Canoroid-v0.1.0-alpha.11-UPDATE.zip)
- [ตรวจสอบ SHA-256](downloads/SHA256SUMS.txt)

แตก ZIP แล้วคัดลอกไฟล์ด้านในไปที่รากการ์ด SD ดูวิธีเตรียมภาพและใช้งานปุ่มกล้องใน [คู่มือ Canoroid](firmware/CANOROID.md)

## การพัฒนา

ซอร์สและเครื่องมือบิลด์อยู่ใน [firmware](firmware/) ตรวจขั้นตอน Live View การบันทึก และการเปิดดูผลบน EOS 600D ก่อนเพิ่ม LUT

Canoroid ใช้ Semantic Versioning โดยใช้แท็ก vMAJOR.MINOR.PATCH และส่วนต่อท้าย alpha.N สำหรับรุ่นทดลอง

## เครดิตและใบอนุญาต

พอร์ตกล้องและระบบพื้นฐานมาจาก Magic Lantern ตัวแปลง JPEG ใช้ซอฟต์แวร์ของ Independent JPEG Group รายละเอียดอยู่ใน [เครดิต](firmware/CANOROID.md#เครดิต) และ [ใบอนุญาตโครงการ](LICENSE)
