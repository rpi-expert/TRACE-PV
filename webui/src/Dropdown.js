import React, { useState } from 'react';

const Dropdown = ({ items, label }) => {
  const [isOpen, setIsOpen] = useState(false);
  const [selectedItem, setSelectedItem] = useState(null);

  const toggleDropdown = () => {
    setIsOpen(!isOpen);
  };

  const handleItemClick = (item) => {
    setSelectedItem(item);
    setIsOpen(false);
  };

  return (
    <div className="dropdown">
      <button onClick={toggleDropdown} className="dropdown-toggle">
        {selectedItem || label}
      </button>
      {isOpen && (
        <ul className="dropdown-menu">
          {items.map((item, index) => (
            <li key={index} onClick={() => handleItemClick(item)} className="dropdown-item">
              {item}
            </li>
          ))}
        </ul>
      )}
    </div>
  );
};

export default Dropdown;
