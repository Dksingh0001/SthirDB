-- SthirDB Phase 7A — constraint-driven retail schema (Yuan 2026 workload, extended).
--
-- One file serves BOTH the constrained and the unconstrained baseline: the
-- session flag `SET constraints_enabled = on|off` (see `sthirdb sql
-- --constraints <on|off>`) decides whether PK/FK/UNIQUE/CHECK/NOT NULL are
-- enforced and whether the constraint-backed unique indexes are created.
-- There is deliberately no second DDL file.
--
-- Normalization (3NF) justification and the CategoryName denormalization
-- trade-off are documented in docs/report.md §9.2.

-- 1. Staff — employees who serve purchases.
CREATE TABLE Staff (
  StaffID INT PRIMARY KEY,
  Name VARCHAR(100) NOT NULL,
  Email VARCHAR(120) NOT NULL UNIQUE
);

-- 2. Shoppers — UNIQUE(Email) backs anomaly class 1 and optimizer rule R1.
CREATE TABLE Shoppers (
  ShopperID INT PRIMARY KEY,
  Name VARCHAR(100) NOT NULL,
  Email VARCHAR(120) NOT NULL UNIQUE,
  City VARCHAR(60),
  JoinDate INT NOT NULL
);

-- 3. Vendors — suppliers of items.
CREATE TABLE Vendors (
  VendorID INT PRIMARY KEY,
  Name VARCHAR(100) NOT NULL,
  Country VARCHAR(60)
);

-- 4. Items — CHECK(StockLevel >= 0) backs anomaly class 2 and rules R2/R2b.
-- CategoryName is denormalized here for read-ability (see report §9.2).
CREATE TABLE Items (
  ItemID INT PRIMARY KEY,
  ItemName VARCHAR(120) NOT NULL,
  CategoryName VARCHAR(60) NOT NULL,
  RetailPrice DECIMAL(10,2) NOT NULL,
  StockLevel INT NOT NULL CHECK (StockLevel >= 0)
);

-- 5. Purchases — one transaction header; ON DELETE RESTRICT on the shopper
-- relationship protects financial history (anomaly class "restrict delete").
CREATE TABLE Purchases (
  PurchaseID INT PRIMARY KEY,
  ShopperID INT NOT NULL REFERENCES Shoppers (ShopperID) ON DELETE RESTRICT,
  StaffID INT NOT NULL REFERENCES Staff (StaffID) ON DELETE RESTRICT,
  PurchaseDate INT NOT NULL,
  Total DECIMAL(12,2) NOT NULL
);

-- 6. Invoices — UNIQUE(PurchaseID): at most one invoice per purchase, and the
-- FK guarantees the purchase exists. ON DELETE CASCADE follows the purchase.
CREATE TABLE Invoices (
  InvoiceID INT PRIMARY KEY,
  PurchaseID INT NOT NULL UNIQUE REFERENCES Purchases (PurchaseID) ON DELETE CASCADE,
  Total DECIMAL(12,2) NOT NULL,
  IssuedDate INT NOT NULL
);

-- 7. Purchase_Line_Items — line items; CHECK(Quantity >= 1) rejects
-- zero/negative quantities; orphan line items are rejected by the FKs.
CREATE TABLE Purchase_Line_Items (
  LineItemID INT PRIMARY KEY,
  PurchaseID INT NOT NULL REFERENCES Purchases (PurchaseID) ON DELETE CASCADE,
  ItemID INT NOT NULL REFERENCES Items (ItemID) ON DELETE RESTRICT,
  Quantity INT NOT NULL CHECK (Quantity >= 1),
  UnitPrice DECIMAL(10,2) NOT NULL,
  LineTotal DECIMAL(12,2) NOT NULL
);

-- 8. Item_Vendor_Catalogs — which vendor supplies which item, at what price.
CREATE TABLE Item_Vendor_Catalogs (
  CatalogID INT PRIMARY KEY,
  ItemID INT NOT NULL REFERENCES Items (ItemID) ON DELETE CASCADE,
  VendorID INT NOT NULL REFERENCES Vendors (VendorID) ON DELETE RESTRICT,
  SupplyPrice DECIMAL(10,2) NOT NULL,
  Quantity INT NOT NULL CHECK (Quantity >= 1)
);
