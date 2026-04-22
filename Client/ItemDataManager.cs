using System.Collections.Generic;
using UnityEngine;

public enum ItemType
{
    Consumable = 0,
    Equipment  = 1,
    Material   = 2,
}

[System.Serializable]
public class ItemData
{
    public ushort   itemID;
    public string   name;
    public ItemType type;
    public int      maxStack;
    public Sprite   icon;
    public string   description;
}

[System.Serializable]
public class ItemDataTable
{
    public List<ItemData> items;
}

public class ItemDataManager : MonoBehaviour
{
    public static ItemDataManager Instance { get; private set; }

    [SerializeField] private string jsonPath = "Data/ItemData";
    [SerializeField] private List<ItemData> itemList = new();

    private Dictionary<ushort, ItemData> _itemMap = new();

    void Awake()
    {
        if (Instance != null) { Destroy(gameObject); return; }
        Instance = this;
        DontDestroyOnLoad(gameObject);
        Load();
    }

    void Load()
    {
        var asset = Resources.Load<TextAsset>(jsonPath);
        if (asset != null)
        {
            var table = JsonUtility.FromJson<ItemDataTable>(asset.text);
            if (table?.items != null)
                itemList = table.items;
        }

        _itemMap.Clear();
        foreach (var item in itemList)
            _itemMap[item.itemID] = item;

        Debug.Log($"[ItemDataManager] {_itemMap.Count}개 아이템 로드 완료");
    }

    public ItemData Get(ushort itemID)
    {
        _itemMap.TryGetValue(itemID, out var data);
        return data;
    }

    public bool Exists(ushort itemID) => _itemMap.ContainsKey(itemID);
}
