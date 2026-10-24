# Retail Domain — ER Diagram (Phase 0 sketch)

Whiteboard-equivalent sketch of the 8-table retail schema from Yuan 2026 (plan Phase 7A). This is the Phase 0 domain sketch; the physical DDL with full constraint syntax lands in Phase 7A.

```mermaid
erDiagram
    Staff ||--o{ Purchases : "creates"
    Shoppers ||--o{ Purchases : "places"
    Purchases ||--|{ Purchase_Line_Items : "contains"
    Purchases ||--o| Invoices : "invoiced_by"
    Items ||--o{ Purchase_Line_Items : "line_items"
    Vendors ||--o{ Item_Vendor_Catalogs : "supplies"
    Items ||--o{ Item_Vendor_Catalogs : "listed_in"

    Staff {
        int StaffID PK
        varchar Name
        varchar Email UK
    }
    Shoppers {
        int ShopperID PK
        varchar Name
        varchar Email UK
    }
    Vendors {
        int VendorID PK
        varchar Name
    }
    Items {
        int ItemID PK
        varchar ItemName
        decimal RetailPrice
        int StockLevel "CHECK >= 0"
    }
    Purchases {
        int PurchaseID PK
        int ShopperID FK
        int StaffID FK
        datetime Timestamp
    }
    Invoices {
        int InvoiceID PK
        int PurchaseID FK "UNIQUE"
        decimal Total
    }
    Item_Vendor_Catalogs {
        int CatalogID PK
        int ItemID FK
        int VendorID FK
        int Quantity "CHECK >= 1"
    }
    Purchase_Line_Items {
        int LineItemID PK
        int PurchaseID FK
        int ItemID FK
        int Quantity "CHECK >= 1"
    }
```

Notes:

- `CHECK (StockLevel >= 0)` on `Items` backs optimizer rule R2/R2b.
- `UNIQUE (Email)` on `Shoppers` backs rule R1.
- `Purchases.ShopperID → Shoppers.ShopperID` (FK + NOT NULL) backs rules R3/R4.
- `ON DELETE RESTRICT` on shopper→purchases; other delete rules decided in Phase 7A.
