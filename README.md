# Canoroid

ถ่ายภาพสไตล์กล้อง instant บน Canon EOS 600D ด้วยฐาน Magic Lantern รุ่นทดลองนี้นำ JPEG จากการ์ดมาครอปตรงกลางเป็น 1:1 เลือกกรอบได้สามแบบ และบันทึกภาพสีขนาด 1080 × 1080 เพิ่มอีกไฟล์

## Canoroid v0.1.0-alpha (รุ่นที่ 6)

สำหรับ **Canon EOS 600D เฟิร์มแวร์ Canon 1.0.2** เลือกกรอบแล้วกด SET เพื่อเข้า Live View ถ่าย JPEG หรือ RAW+JPEG ทีละภาพ แล้วเปรียบเทียบต้นฉบับกับภาพใส่กรอบได้ในหน้าผลลัพธ์ ผู้ใช้ยืนยันว่ารุ่นที่ 5 ถ่าย ใส่กรอบ และแสดงภาพเปรียบเทียบครบหนึ่งรอบบนกล้องจริง รุ่นที่ 6 แก้การถ่ายต่อหลังกลับหน้าหลัก และเพิ่ม View photos สำหรับเปิดดูภาพใส่กรอบจากการ์ด รวมทั้งไฟล์ P0001.JPG ของรุ่นก่อน ภาพใหม่ใช้เลขโฟลเดอร์และเลขภาพของ Canon เช่น 100_0123.JPG การถ่ายซ้ำและเปิดการ์ดเดิมผ่านการทดสอบบนคอมพิวเตอร์ ส่วนรุ่นที่ 6 ยังรอยืนยันบนตัวกล้อง

- ติดตั้งครั้งแรก มีตัวติดตั้ง ML-SETUP.FIR — [ดาวน์โหลด FIRST-INSTALL](https://raw.githubusercontent.com/pongallery/Canoroid/v0.1.0-alpha.6/downloads/Canoroid-v0.1.0-alpha.6-FIRST-INSTALL.zip)
- อัปเดตกล้องและการ์ดที่บูต Magic Lantern ได้อยู่แล้ว — [ดาวน์โหลด UPDATE](https://raw.githubusercontent.com/pongallery/Canoroid/v0.1.0-alpha.6/downloads/Canoroid-v0.1.0-alpha.6-UPDATE.zip)
- [ตรวจสอบ SHA-256](downloads/SHA256SUMS.txt)

แตก ZIP แล้วคัดลอกไฟล์ด้านในไปที่รากการ์ด SD ดูวิธีเตรียมภาพและใช้งานปุ่มกล้องใน [คู่มือ Canoroid](firmware/CANOROID.md)

## การพัฒนา

ซอร์สและเครื่องมือบิลด์อยู่ใน [firmware](firmware/) ตรวจขั้นตอน Live View การบันทึก และการเปิดดูผลบน EOS 600D ก่อนเพิ่ม LUT

Canoroid ใช้ Semantic Versioning โดยใช้แท็ก vMAJOR.MINOR.PATCH และส่วนต่อท้าย alpha.N สำหรับรุ่นทดลอง

## เครดิตและใบอนุญาต

พอร์ตกล้องและระบบพื้นฐานมาจาก Magic Lantern ตัวแปลง JPEG ใช้ซอฟต์แวร์ของ Independent JPEG Group รายละเอียดอยู่ใน [เครดิต](firmware/CANOROID.md#เครดิต) และ [ใบอนุญาตโครงการ](LICENSE)
